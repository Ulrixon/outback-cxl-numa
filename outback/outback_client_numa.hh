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

// Server state handle for direct access
ServerNumaState* remote_server_state = nullptr;
SharedNumaRegistry* remote_registry = nullptr;
void* remote_region_base = nullptr;
int remote_region_fd = -1;
int remote_registry_fd = -1;

ludo_buckets_t* remote_ludo_buckets = nullptr;
packed_data_t* remote_packed_data = nullptr;
volatile uint8_t* remote_lock_array = nullptr;
thread_local uint32_t remote_lane_id = 0;

inline void set_remote_lane_id(uint32_t lane) {
    remote_lane_id = lane;
}

inline auto alloc_remote_data_slot() -> ::r2::Option<size_t> {
    if (!remote_registry) {
        return {};
    }

    const uint32_t lanes = (remote_registry->mem_threads == 0)
        ? 1
        : std::min<uint32_t>(remote_registry->mem_threads, static_cast<uint32_t>(kMaxNumaLanes));
    const uint32_t lane = remote_lane_id % lanes;

    auto idx = __sync_fetch_and_add(&(remote_registry->lane_next_free_index[lane]), static_cast<size_t>(lanes));
    if (idx >= remote_registry->num_data_entries) {
        return {};
    }
    return idx;
}

namespace outback {

/**
 * @brief Direct memory access to server's NUMA memory (replaces remote_search via RDMA)
 */
inline auto numa_search(const KeyType& key) -> ::r2::Option<ValType>
{
    if (!remote_ludo_buckets || !remote_packed_data) {
        return {};
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    auto addr = remote_ludo_buckets->read_addr(loc.first, loc.second);
    ValType value = remote_packed_data->rawArray[addr].data;
    
    return value;
}

/**
 * @brief Direct memory access PUT operation (replaces remote_put via RDMA)
 */
inline void numa_put(const KeyType& key, const ValType& val)
{
    if (!remote_ludo_buckets || !remote_packed_data || !remote_lock_array) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;

    numa_lock_byte(&remote_lock_array[row]);
    
    FastHasher64<K> h;
    h.setSeed(ludo_lookup_unit->buckets[row].seed);
    uint8_t slot = uint8_t(h(key) >> 62);
    
    int8_t status = remote_ludo_buckets->check_slots(key, row, slot);
    
    switch (status) {
        case 0: {
            // Empty slot - insert new entry
            auto slot_addr = alloc_remote_data_slot();
            if (!slot_addr) {
                break;
            }
            remote_packed_data->update_data(*slot_addr, key, sizeof(ValType), val);
            remote_ludo_buckets->write_addr(key, row, slot, *slot_addr);
            break;
        }
        case 1: {
            // Update existing entry
            auto addr = remote_ludo_buckets->read_addr(row, slot);
            KeyType key_;
            remote_packed_data->read_key(addr, key_);
            if (likely(key_ == key)) {
                remote_packed_data->update_data(addr, key, 64, val);
            }
            break;
        }
        case 2: {
            // Bucket has empty slots
            for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
                if (remote_ludo_buckets->check_slots(key, row, s) == 0) {
                    auto slot_addr = alloc_remote_data_slot();
                    if (!slot_addr) {
                        break;
                    }
                    remote_packed_data->update_data(*slot_addr, key, sizeof(ValType), val);
                    remote_ludo_buckets->write_addr(key, row, s, *slot_addr);
                    break;
                }
            }
            break;
        }
        case 3: {
            // Bucket is full - need to handle overflow
            // For now, just update the first slot
            auto addr = remote_ludo_buckets->read_addr(row, slot);
            remote_packed_data->update_data(addr, key, 64, val);
            // Update seed if needed
            ludo_lookup_unit->updateSeed(row, ludo_lookup_unit->buckets[row].seed + 1);
            break;
        }
    }

    numa_unlock_byte(&remote_lock_array[row]);
}

/**
 * @brief Direct memory access UPDATE operation (replaces remote_update via RDMA)
 */
inline void numa_update(const KeyType& key, const ValType& val)
{
    if (!remote_ludo_buckets || !remote_packed_data || !remote_lock_array) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    
    numa_lock_byte(&remote_lock_array[row]);
    
    auto addr = remote_ludo_buckets->read_addr(row, loc.second);
    KeyType key_;
    remote_packed_data->read_key(addr, key_);
    
    if (likely(key_ == key)) {
        remote_packed_data->update_data(addr, key, 64, val);
    }

    numa_unlock_byte(&remote_lock_array[row]);
}

/**
 * @brief Direct memory access REMOVE operation (replaces remote_remove via RDMA)
 */
inline void numa_remove(const KeyType& key)
{
    if (!remote_ludo_buckets || !remote_packed_data || !remote_lock_array) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    
    numa_lock_byte(&remote_lock_array[row]);
    
    auto addr = remote_ludo_buckets->read_addr(row, loc.second);
    auto res = remote_packed_data->remove_data_with_key_check(addr, key);
    if (res) {
        remote_ludo_buckets->remove_addr(row, loc.second);
    }

    numa_unlock_byte(&remote_lock_array[row]);
}

/**
 * @brief Direct memory access SCAN operation (replaces remote_scan via RDMA)
 */
inline std::vector<V> numa_scan(const KeyType& key, const u64& n)
{
    std::vector<V> result;
    if (!remote_ludo_buckets || !remote_packed_data) {
        return result;
    }

    // Simplified scan implementation
    // In a real implementation, this would iterate through the data structure
    return result;
}

/**
 * @brief Connect to server NUMA memory
 */
inline bool connect_to_numa_server(const std::string& server_name = "default") {
    remote_server_state = NumaServerManager::instance().get_server_state(server_name);
    if (remote_server_state) {
        remote_ludo_buckets = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
        remote_packed_data = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
        remote_lock_array = remote_server_state->lock_array;
        LOG(2) << "Connected to in-process NUMA server: " << server_name
               << " on node " << remote_server_state->numa_node;
        return true;
    }

    if (!map_numa_registry(server_name, false, &remote_registry, &remote_registry_fd)) {
        LOG(4) << "Failed to open NUMA shared registry for server: " << server_name;
        return false;
    }

    if (remote_registry->magic != kNumaMetaMagic || remote_registry->ready == 0) {
        LOG(4) << "NUMA shared registry not ready for server: " << server_name;
        if (remote_registry) {
            munmap(remote_registry, sizeof(SharedNumaRegistry));
            remote_registry = nullptr;
        }
        if (remote_registry_fd >= 0) {
            close(remote_registry_fd);
            remote_registry_fd = -1;
        }
        return false;
    }

    if (!open_shared_numa_region(server_name, remote_registry->region_size, &remote_region_base, &remote_region_fd)) {
        LOG(4) << "Failed to map NUMA shared region for server: " << server_name;
        if (remote_registry) {
            munmap(remote_registry, sizeof(SharedNumaRegistry));
            remote_registry = nullptr;
        }
        if (remote_registry_fd >= 0) {
            close(remote_registry_fd);
            remote_registry_fd = -1;
        }
        return false;
    }

    char* base = reinterpret_cast<char*>(remote_region_base);
    auto* ludo_mem = reinterpret_cast<LudoBucket*>(base + remote_registry->ludo_buckets_offset);
    remote_ludo_buckets = new ludo_buckets_t(ludo_mem, remote_registry->num_buckets);
    remote_packed_data = new packed_data_t(base + remote_registry->packed_array_offset,
                                           remote_registry->num_data_entries);
    remote_lock_array = reinterpret_cast<volatile uint8_t*>(base + remote_registry->lock_array_offset);

    LOG(2) << "Connected to shared NUMA server: " << server_name
           << " on node " << remote_registry->numa_node;
    return true;
}

} // namespace outback
