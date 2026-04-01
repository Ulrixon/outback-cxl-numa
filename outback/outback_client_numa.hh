#pragma once

#include "outback/trait_numa.hpp"
#include "xcomm/src/transport/numa_transport.hh"

using namespace r2;
using namespace xstore::util;
using namespace xstore::transport;
using namespace xstore::numa;
using namespace outback;

/*****  GLOBAL VARIABLES *******/
ludo_lookup_t* ludo_lookup_unit;  // act like seeds

// Server state handle for direct access (in-process fast path)
ServerNumaState* remote_server_state = nullptr;

// Shared-memory registry (cross-process access)
SharedNumaRegistry* remote_registry  = nullptr;
int remote_registry_fd               = -1;

ludo_buckets_t*    remote_ludo_buckets = nullptr;
packed_data_t*     remote_packed_data  = nullptr;
volatile uint8_t*  remote_lock_array   = nullptr;

thread_local uint32_t remote_lane_id = 0;

// ─── Per-client version/region state ─────────────────────────────────────────
// Tracks the client's local view of the current versioned region so that
// stale-version detection and transparent remap work correctly.

struct ClientRegionState {
    uint64_t local_version    = 0;
    uint64_t local_generation = 0;

    int    region_fd          = -1;
    void*  region_base        = nullptr;

    ludo_buckets_t*   ludo_buckets = nullptr;
    packed_data_t*    packed_data  = nullptr;
    volatile uint8_t* lock_array   = nullptr;

    int client_slot = -1; // slot in registry->clients[]
};

static thread_local ClientRegionState t_region{};
static std::string g_client_server_name; // set once on connect

inline void set_remote_lane_id(uint32_t lane) { remote_lane_id = lane; }

// ─── Allocation ───────────────────────────────────────────────────────────────

inline auto alloc_remote_data_slot() -> ::r2::Option<size_t> {
    if (!remote_registry) return {};

    const uint32_t lanes = (remote_registry->mem_threads == 0)
        ? 1
        : std::min<uint32_t>(remote_registry->mem_threads,
                              static_cast<uint32_t>(kMaxNumaLanes));
    const uint32_t lane = remote_lane_id % lanes;

    const auto& ep = current_epoch(*remote_registry);
    auto idx = __sync_fetch_and_add(
        &(remote_registry->lane_next_free_index[lane]),
        static_cast<size_t>(lanes));
    if (idx >= ep.num_data_entries) return {};
    return idx;
}

// ─── Heartbeat ────────────────────────────────────────────────────────────────

inline void client_heartbeat() {
    if (!remote_registry) return;
    if (t_region.client_slot < 0) return;
    uint64_t now_ns;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
             static_cast<uint64_t>(ts.tv_nsec);
    remote_registry->clients[t_region.client_slot].last_heartbeat_ns.store(
        now_ns, std::memory_order_relaxed);
    remote_registry->clients[t_region.client_slot].last_seen_generation.store(
        remote_registry->generation.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

namespace outback {

// ─── Internal: remap to the current epoch's shm region ───────────────────────

inline bool remap_to_current_epoch(const std::string& server_name) {
    if (!remote_registry) return false;

    uint64_t new_ver = remote_registry->current_version.load(std::memory_order_acquire);
    const auto& ep   = remote_registry->epoch[new_ver % kMaxEpochSlots];

    // Wait until epoch is published with acquire semantics
    if (ep.published.load(std::memory_order_acquire) == 0) return false;

    // Unmap old region
    if (t_region.region_base && t_region.region_fd >= 0) {
        // Old ludo_buckets/packed_data wrappers were created with new; delete them
        if (t_region.ludo_buckets && t_region.ludo_buckets != remote_ludo_buckets) {
            delete t_region.ludo_buckets;
        }
        if (t_region.packed_data && t_region.packed_data != remote_packed_data) {
            delete t_region.packed_data;
        }
        munmap(t_region.region_base, ep.region_size /* old size – approx */);
        close(t_region.region_fd);
    }

    // Map new versioned region
    void* new_base = nullptr;
    int   new_fd   = -1;
    auto& new_ep = remote_registry->epoch[new_ver % kMaxEpochSlots]; // non-const: fetch_sub below
    if (!open_shared_numa_region(server_name, new_ep.region_size, new_ver,
                                 &new_base, &new_fd)) {
        return false;
    }

    char* base     = reinterpret_cast<char*>(new_base);
    auto* ludo_mem = reinterpret_cast<LudoBucket*>(base + new_ep.ludo_buckets_offset);

    t_region.region_base  = new_base;
    t_region.region_fd    = new_fd;
    t_region.ludo_buckets = new ludo_buckets_t(ludo_mem, new_ep.num_buckets);
    t_region.packed_data  = new packed_data_t(base + new_ep.packed_array_offset,
                                              new_ep.num_data_entries);
    t_region.lock_array   = reinterpret_cast<volatile uint8_t*>(
                                base + new_ep.lock_array_offset);
    t_region.local_version    = new_ver;
    t_region.local_generation = remote_registry->generation.load(
                                    std::memory_order_relaxed);

    // Also update global pointers for single-threaded / legacy paths
    remote_ludo_buckets = t_region.ludo_buckets;
    remote_packed_data  = t_region.packed_data;
    remote_lock_array   = t_region.lock_array;

    // Acknowledge to server
    new_ep.clients_pending_ack.fetch_sub(1, std::memory_order_acq_rel);

    // Update heartbeat
    client_heartbeat();
    return true;
}

// ─── Check if a remap is needed (called on hot path every N ops) ─────────────

inline void maybe_check_version() {
    if (!remote_registry) return;

    uint64_t cur_gen = remote_registry->generation.load(std::memory_order_acquire);
    if (cur_gen == t_region.local_generation) return; // fast path: no change

    uint64_t cur_ver = remote_registry->current_version.load(std::memory_order_acquire);
    if (cur_ver != t_region.local_version) {
        remap_to_current_epoch(g_client_server_name);
    } else {
        // Generation changed but version is the same (e.g. PRE_RESIZE signal)
        t_region.local_generation = cur_gen;
        client_heartbeat();
    }
}

// ─── Data operations ──────────────────────────────────────────────────────────

inline auto numa_search(const KeyType& key) -> ::r2::Option<ValType>
{
    maybe_check_version();

    auto* lbs = remote_ludo_buckets;
    auto* pd  = remote_packed_data;
    if (!lbs || !pd) return {};

    auto loc  = ludo_lookup_unit->lookup_slot(key);
    auto addr = lbs->read_addr(loc.first, loc.second);

    // Fast path: key matches at the primary slot
    KeyType stored_key;
    pd->read_key(addr, stored_key);
    if (stored_key == key) {
        ValType v = pd->rawArray[addr].data; return v; // copy: packed field can't bind to ref
    }

    // ── MakeupGet (plan §14) ──────────────────────────────────────────────────
    // The primary slot fingerprint collided with a different key.  This can
    // happen transiently during a resize when the Ludo seed hasn't been updated
    // yet, or due to a genuine fingerprint collision.  Scan all SLOTS_NUM_BUCKET
    // slots in the same row as a fallback before declaring NOT_FOUND.
    size_t row = loc.first;
    for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; ++s) {
        if (s == static_cast<uint8_t>(loc.second)) continue; // already checked
        auto a = lbs->read_addr(row, s);
        if (a == 0) continue; // empty slot (addr 0 means unoccupied)
        KeyType k;
        pd->read_key(a, k);
        if (k == key) {
            ValType v = pd->rawArray[a].data; return v; // copy: packed field can't bind to ref
        }
    }

    // LRU overflow cache lives server-side only; not accessible from client.

    return {};
}

inline void numa_put(const KeyType& key, const ValType& val)
{
    maybe_check_version();

    auto* lbs  = remote_ludo_buckets;
    auto* pd   = remote_packed_data;
    auto* lock = remote_lock_array;
    if (!lbs || !pd || !lock) return;

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;

    numa_lock_byte(&lock[row]);

    FastHasher64<K> h;
    h.setSeed(ludo_lookup_unit->buckets[row].seed);
    uint8_t slot = uint8_t(h(key) >> 62);

    int8_t status = lbs->check_slots(key, row, slot);
    switch (status) {
        case 0: {
            auto slot_addr = alloc_remote_data_slot();
            if (slot_addr) {
                pd->update_data(*slot_addr, key, sizeof(ValType), val);
                lbs->write_addr(key, row, slot, *slot_addr);
            }
            break;
        }
        case 1: {
            auto addr = lbs->read_addr(row, slot);
            KeyType key_;
            pd->read_key(addr, key_);
            if (likely(key_ == key)) pd->update_data(addr, key, 64, val);
            break;
        }
        case 2: {
            for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
                if (lbs->check_slots(key, row, s) == 0) {
                    auto slot_addr = alloc_remote_data_slot();
                    if (slot_addr) {
                        pd->update_data(*slot_addr, key, sizeof(ValType), val);
                        lbs->write_addr(key, row, s, *slot_addr);
                    }
                    break;
                }
            }
            break;
        }
        case 3: {
            auto addr = lbs->read_addr(row, slot);
            pd->update_data(addr, key, 64, val);
            ludo_lookup_unit->updateSeed(row, ludo_lookup_unit->buckets[row].seed + 1);
            break;
        }
    }
    numa_unlock_byte(&lock[row]);
}

inline void numa_update(const KeyType& key, const ValType& val)
{
    maybe_check_version();

    auto* lbs  = remote_ludo_buckets;
    auto* pd   = remote_packed_data;
    auto* lock = remote_lock_array;
    if (!lbs || !pd || !lock) return;

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    numa_lock_byte(&lock[row]);
    auto addr = lbs->read_addr(row, loc.second);
    KeyType key_;
    pd->read_key(addr, key_);
    if (likely(key_ == key)) pd->update_data(addr, key, 64, val);
    numa_unlock_byte(&lock[row]);
}

inline void numa_remove(const KeyType& key)
{
    maybe_check_version();

    auto* lbs  = remote_ludo_buckets;
    auto* pd   = remote_packed_data;
    auto* lock = remote_lock_array;
    if (!lbs || !pd || !lock) return;

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    numa_lock_byte(&lock[row]);
    auto addr = lbs->read_addr(row, loc.second);
    auto res  = pd->remove_data_with_key_check(addr, key);
    if (res) lbs->remove_addr(row, loc.second);
    numa_unlock_byte(&lock[row]);
}

inline std::vector<V> numa_scan(const KeyType& /*key*/, const u64& /*n*/)
{
    // Simplified placeholder
    return {};
}

// ─── Connect to server ────────────────────────────────────────────────────────

inline bool connect_to_numa_server(const std::string& server_name = "default") {
    g_client_server_name = server_name;

    // Fast path: in-process server
    remote_server_state = NumaServerManager::instance().get_server_state(server_name);
    if (remote_server_state) {
        remote_ludo_buckets  = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
        remote_packed_data   = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
        remote_lock_array    = remote_server_state->lock_array;

        // Populate thread-local state for version tracking
        remote_registry = remote_server_state->shared_meta;
        if (remote_registry) {
            uint64_t ver = remote_registry->current_version.load(std::memory_order_acquire);
            t_region.local_version    = ver;
            t_region.local_generation = remote_registry->generation.load(std::memory_order_relaxed);
            t_region.ludo_buckets     = remote_ludo_buckets;
            t_region.packed_data      = remote_packed_data;
            t_region.lock_array       = remote_lock_array;
        }
        LOG(2) << "Connected to in-process NUMA server: " << server_name
               << " on node " << remote_server_state->numa_node;
        return true;
    }

    // Cross-process path: open shm registry
    if (!map_numa_registry(server_name, false, &remote_registry, &remote_registry_fd)) {
        LOG(4) << "Failed to open NUMA shared registry for server: " << server_name;
        return false;
    }

    if (remote_registry->magic != kNumaMetaMagic || remote_registry->ready == 0) {
        munmap(remote_registry, sizeof(SharedNumaRegistry));
        remote_registry    = nullptr;
        if (remote_registry_fd >= 0) { close(remote_registry_fd); remote_registry_fd = -1; }
        return false;
    }

    // Wait until epoch is published
    uint64_t cur_ver = remote_registry->current_version.load(std::memory_order_acquire);
    auto& ep   = remote_registry->epoch[cur_ver % kMaxEpochSlots]; // non-const: fetch_sub below
    if (ep.published.load(std::memory_order_acquire) == 0) {
        munmap(remote_registry, sizeof(SharedNumaRegistry));
        remote_registry    = nullptr;
        if (remote_registry_fd >= 0) { close(remote_registry_fd); remote_registry_fd = -1; }
        return false;
    }

    // Register client heartbeat slot
    for (size_t i = 0; i < kMaxClients; ++i) {
        uint32_t expected = 0;
        if (remote_registry->clients[i].active.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            t_region.client_slot = static_cast<int>(i);
            remote_registry->registered_clients.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    // Map the current versioned data region
    void* region_base = nullptr;
    int   region_fd   = -1;
    if (!open_shared_numa_region(server_name, ep.region_size, cur_ver,
                                 &region_base, &region_fd)) {
        munmap(remote_registry, sizeof(SharedNumaRegistry));
        remote_registry    = nullptr;
        if (remote_registry_fd >= 0) { close(remote_registry_fd); remote_registry_fd = -1; }
        return false;
    }

    char* base     = reinterpret_cast<char*>(region_base);
    auto* ludo_mem = reinterpret_cast<LudoBucket*>(base + ep.ludo_buckets_offset);

    t_region.region_base      = region_base;
    t_region.region_fd        = region_fd;
    t_region.ludo_buckets     = new ludo_buckets_t(ludo_mem, ep.num_buckets);
    t_region.packed_data      = new packed_data_t(base + ep.packed_array_offset,
                                                  ep.num_data_entries);
    t_region.lock_array       = reinterpret_cast<volatile uint8_t*>(
                                    base + ep.lock_array_offset);
    t_region.local_version    = cur_ver;
    t_region.local_generation = remote_registry->generation.load(std::memory_order_relaxed);

    remote_ludo_buckets = t_region.ludo_buckets;
    remote_packed_data  = t_region.packed_data;
    remote_lock_array   = t_region.lock_array;

    // Acknowledge to server
    ep.clients_pending_ack.fetch_sub(1, std::memory_order_acq_rel);
    client_heartbeat();

    LOG(2) << "Connected to shared NUMA server: " << server_name
           << " on node " << remote_registry->numa_node
           << " (version " << cur_ver << ")";
    return true;
}

} // namespace outback
