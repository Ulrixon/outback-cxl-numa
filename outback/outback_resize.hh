#pragma once

/*
 * outback_resize.hh
 *
 * Server-side resize orchestrator for the Outback NUMA transport.
 *
 * Implements the full resize state machine from NUMA_RESIZING_PLAN.md:
 *   NORMAL → PRE_RESIZE → COPYING → READY_TO_SWITCH → DRAIN_OLD → GC_PENDING → NORMAL
 *
 * Key design points:
 *   - Versioned shm regions: old region kept alive until all clients ack and
 *     grace period expires.
 *   - Deferred Insert/Delete queues: structural mutations blocked during COPYING
 *     are captured and replayed into the new region during DRAIN_OLD.
 *   - Heartbeat-based zombie client detection: a dead client cannot block GC
 *     forever.
 *   - Memory pressure guard: refuses to start COPYING if NUMA RAM is
 *     insufficient for concurrent old + new region overlap.
 *   - Generation counter incremented on every state transition so clients can
 *     detect changes cheaply.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <thread>
#include <unistd.h>

#include "xcomm/src/transport/numa_transport.hh"
#include "outback/ludo_slot.hh"
#include "outback/packed_data.hh"
#include "outback/trait_numa.hpp"
#include "ludo/hashutils/hash.h"

namespace outback {

using namespace xstore::transport;

// ─── Deferred mutation queues ────────────────────────────────────────────────
//
// During COPYING/DRAIN_OLD, Insert and Delete requests are enqueued here.
// After the new region is built and drain begins, they are replayed in order.

template<typename K, typename V>
struct DeferredInsert {
    K        key;
    V        value;
    uint64_t enqueue_ts_ns;
};

template<typename K>
struct DeferredDelete {
    K        key;
    uint64_t enqueue_ts_ns;
};

// ─── Resize metrics (observability) ──────────────────────────────────────────

struct ResizeMetrics {
    uint64_t resize_count         = 0;
    uint64_t copy_duration_ms     = 0;
    uint64_t ack_wait_ms          = 0;
    uint64_t drain_duration_ms    = 0;
    uint64_t gc_delay_ms          = 0;
    uint64_t zombie_cleanups      = 0;
    uint64_t pressure_guard_fails = 0;
    uint64_t deferred_inserts_total = 0;
    uint64_t deferred_deletes_total = 0;
};

// ─── ResizeOrchestrator ───────────────────────────────────────────────────────

class ResizeOrchestrator {
public:
    // How many nanoseconds without a heartbeat before a client is zombie
    static constexpr uint64_t kZombieThresholdNs = 30ULL * 1'000'000'000ULL; // 30 s
    // How long to wait for stragglers before force-continuing
    static constexpr uint64_t kAckGraceMs        = 5'000; // 5 s
    // How long after GC_PENDING before actually reclaiming
    static constexpr uint64_t kGcGraceMs         = 2'000; // 2 s
    // Maximum deferred queue depth
    static constexpr size_t   kMaxDeferredDepth  = 1'000'000;

    ResizeOrchestrator(SharedNumaRegistry* reg,
                       ServerNumaState*    srv_state,
                       const std::string&  server_name,
                       int                 numa_node,
                       ludo_lookup_t*      ludo_lookup,
                       std::mutex*         mutex_array)
        : reg_(reg), srv_state_(srv_state),
          server_name_(server_name), numa_node_(numa_node),
          ludo_lookup_(ludo_lookup), mutex_array_(mutex_array) {}

    ~ResizeOrchestrator() { stop(); }

    // Start background monitoring thread
    void start() {
        running_ = true;

        // Register interception globals so op handlers (outback_server_numa.hh)
        // can defer mutations without knowing this class's full definition.
        xstore::transport::g_resize_orch_ptr = this;
        xstore::transport::g_defer_insert_fn = [](void* orch, uint64_t key, uint64_t val) -> bool {
            return reinterpret_cast<ResizeOrchestrator*>(orch)->defer_insert(key, val);
        };
        xstore::transport::g_defer_delete_fn = [](void* orch, uint64_t key) -> bool {
            return reinterpret_cast<ResizeOrchestrator*>(orch)->defer_delete(key);
        };

        thread_  = std::thread(&ResizeOrchestrator::monitor_loop, this);
    }

    // Stop and join the background thread
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();

        // Clear interception globals
        xstore::transport::g_structural_mutations_blocked.store(false,
            std::memory_order_release);
        xstore::transport::g_resize_orch_ptr   = nullptr;
        xstore::transport::g_defer_insert_fn   = nullptr;
        xstore::transport::g_defer_delete_fn   = nullptr;
    }

    const ResizeMetrics& metrics() const { return metrics_; }

    // ── Called by server op handlers to check/defer mutations ──────────────

    // Returns true if the current resize state allows structural mutations
    // (Insert / Delete).  If false, the caller should enqueue or return RETRY.
    bool structural_mutations_allowed() const {
        auto state = static_cast<ResizeState>(
            reg_->resize_state.load(std::memory_order_acquire));
        return state == ResizeState::NORMAL || state == ResizeState::PRE_RESIZE;
    }

    // Enqueue a deferred insert.  Returns false if queue is full (backpressure).
    bool defer_insert(const KeyType& key, const ValType& val) {
        std::lock_guard<std::mutex> lk(defer_mu_);
        if (deferred_inserts_.size() >= kMaxDeferredDepth) return false;
        deferred_inserts_.push_back({key, val, now_ns()});
        reg_->deferred_insert_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.deferred_inserts_total++;
        return true;
    }

    // Enqueue a deferred delete.  Returns false if queue is full.
    bool defer_delete(const KeyType& key) {
        std::lock_guard<std::mutex> lk(defer_mu_);
        if (deferred_deletes_.size() >= kMaxDeferredDepth) return false;
        deferred_deletes_.push_back({key, now_ns()});
        reg_->deferred_delete_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.deferred_deletes_total++;
        return true;
    }

    // ── Client heartbeat helpers ────────────────────────────────────────────

    // Called by clients on connect; returns client slot index or -1
    int register_client() {
        for (size_t i = 0; i < kMaxClients; ++i) {
            uint32_t expected = 0;
            if (reg_->clients[i].active.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
                uint64_t cur_gen = reg_->generation.load(std::memory_order_relaxed);
                reg_->clients[i].last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
                reg_->clients[i].last_seen_generation.store(cur_gen, std::memory_order_relaxed);
                reg_->registered_clients.fetch_add(1, std::memory_order_relaxed);
                return static_cast<int>(i);
            }
        }
        return -1; // no slot available
    }

    // Called by client periodically (or before any operation)
    void update_heartbeat(int client_slot) {
        if (client_slot < 0 || static_cast<size_t>(client_slot) >= kMaxClients) return;
        reg_->clients[client_slot].last_heartbeat_ns.store(now_ns(),
                                                           std::memory_order_relaxed);
        reg_->clients[client_slot].last_seen_generation.store(
            reg_->generation.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    // Called on client disconnect
    void deregister_client(int client_slot) {
        if (client_slot < 0 || static_cast<size_t>(client_slot) >= kMaxClients) return;
        uint32_t expected = 1;
        if (reg_->clients[client_slot].active.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            reg_->registered_clients.fetch_sub(1, std::memory_order_relaxed);
        }
    }

private:
    // ── Background monitor loop ───────────────────────────────────────────────

    void monitor_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!running_) break;
            check_state();
        }
    }

    void check_state() {
        auto state = static_cast<ResizeState>(
            reg_->resize_state.load(std::memory_order_acquire));

        switch (state) {
            case ResizeState::NORMAL:
                maybe_trigger_pre_resize();
                break;
            case ResizeState::PRE_RESIZE:
                maybe_start_copying();
                break;
            case ResizeState::COPYING:
                // copying is driven synchronously by do_copy(); nothing to do here
                break;
            case ResizeState::READY_TO_SWITCH:
                wait_for_acks_or_timeout();
                break;
            case ResizeState::DRAIN_OLD:
                drain_old();
                break;
            case ResizeState::GC_PENDING:
                maybe_gc();
                break;
        }
    }

    // ── NORMAL → PRE_RESIZE ───────────────────────────────────────────────────

    void maybe_trigger_pre_resize() {
        if (reg_->s_slow == 0) return; // thresholds not configured; skip

        uint64_t cur_ver = reg_->current_version.load(std::memory_order_acquire);
        const auto& ep   = reg_->epoch[cur_ver % kMaxEpochSlots];

        // Estimate current fill: use the highest lane_next_free_index
        size_t max_idx = 0;
        uint32_t lanes = reg_->mem_threads ? reg_->mem_threads : 1;
        for (uint32_t i = 0; i < lanes; ++i) {
            size_t idx = __sync_fetch_and_add(&reg_->lane_next_free_index[i], 0);
            if (idx > max_idx) max_idx = idx;
        }

        if (max_idx >= reg_->s_slow) {
            set_state(ResizeState::PRE_RESIZE);
        }
    }

    // ── PRE_RESIZE → COPYING ─────────────────────────────────────────────────

    void maybe_start_copying() {
        if (reg_->s_slow == 0) return;

        uint64_t cur_ver = reg_->current_version.load(std::memory_order_acquire);
        const auto& ep   = reg_->epoch[cur_ver % kMaxEpochSlots];

        size_t max_idx = 0;
        uint32_t lanes = reg_->mem_threads ? reg_->mem_threads : 1;
        for (uint32_t i = 0; i < lanes; ++i) {
            size_t idx = __sync_fetch_and_add(&reg_->lane_next_free_index[i], 0);
            if (idx > max_idx) max_idx = idx;
        }

        // Only enter COPYING once s_stop is crossed, or if we've been in
        // PRE_RESIZE for too long (future: add timer).
        if (max_idx < reg_->s_stop) return;

        // Memory pressure guard: ensure NUMA node can hold 2× current region
        if (!memory_pressure_ok(ep.region_size)) {
            metrics_.pressure_guard_fails++;
            return;
        }

        do_copy(cur_ver);
    }

    // ── Core copy step ────────────────────────────────────────────────────────

    void do_copy(uint64_t old_ver) {
        set_state(ResizeState::COPYING);

        auto t0 = now_ms();
        uint64_t new_ver = old_ver + 1;
        if (new_ver % kMaxEpochSlots == 0) new_ver++; // skip slot 0

        const auto& old_ep = reg_->epoch[old_ver % kMaxEpochSlots];

        // Target region: 2× the old size for growth headroom
        size_t new_data_entries = old_ep.num_data_entries * 2;
        size_t new_num_buckets  = old_ep.num_buckets * 2; // extendible hashing growth

        size_t packed_size   = new_data_entries * sizeof(packed_struct_t);
        // LudoBucket size is opaque; use same per-bucket size as old region
        size_t bucket_size_each = (old_ep.num_buckets > 0)
            ? (old_ep.lock_array_offset - old_ep.ludo_buckets_offset) / old_ep.num_buckets
            : sizeof(LudoBucket);
        size_t ludo_size     = new_num_buckets * bucket_size_each;
        size_t lock_size     = new_num_buckets * sizeof(uint8_t);

        size_t packed_off  = align_up(0, 64);
        size_t ludo_off    = align_up(packed_off + packed_size, 64);
        size_t lock_off    = align_up(ludo_off  + ludo_size,   64);
        size_t total_size  = align_up(lock_off  + lock_size,   4096);

        // Create new shm region
        void* new_base = nullptr;
        int   new_fd   = -1;
        if (!create_shared_numa_region(server_name_, total_size, numa_node_,
                                       new_ver, &new_base, &new_fd)) {
            // Failed to allocate; abort and return to NORMAL
            set_state(ResizeState::NORMAL);
            return;
        }
        new_region_base_ = new_base;
        new_region_fd_   = new_fd;

        // Copy payload from old region into new
        char* old_base_ptr = reinterpret_cast<char*>(srv_state_->shared_region_base);
        char* new_base_ptr = reinterpret_cast<char*>(new_base);

        // Copy existing packed entries verbatim into the lower half of the new
        // packed array.  The upper half (indices num_data_entries..2×-1) starts
        // zero-initialized by the OS (ftruncate fills with zero); new inserts
        // will be placed there via the reset lane_next_free_index values.
        std::memcpy(new_base_ptr + packed_off,
                    old_base_ptr + old_ep.packed_array_offset,
                    old_ep.num_data_entries * sizeof(packed_struct_t));

        // Extendible hashing bucket layout after doubling:
        //   Slots [0, N)   → copy of old directory; serve existing keys unchanged.
        //   Slots [N, 2N)  → zero-initialized; populated as new inserts arrive.
        //
        // The OS guarantees ftruncate fills with zeros, so the upper half is
        // already clean.  We copy only the old buckets into the lower half.
        std::memcpy(new_base_ptr + ludo_off,
                    old_base_ptr + old_ep.ludo_buckets_offset,
                    old_ep.num_buckets * bucket_size_each);
        // Explicitly zero the upper half for clarity (and defensive correctness
        // in case the shm was recycled without going through ftruncate).
        std::memset(new_base_ptr + ludo_off + old_ep.num_buckets * bucket_size_each,
                    0,
                    old_ep.num_buckets * bucket_size_each);

        // Zero the lock array
        std::memset(new_base_ptr + lock_off, 0, lock_size);

        // ── Publish new epoch ─────────────────────────────────────────────────
        auto& new_ep = reg_->epoch[new_ver % kMaxEpochSlots];
        new_ep.version           = new_ver;
        new_ep.publish_ts_ns     = now_ns();
        new_ep.region_size       = total_size;
        new_ep.num_buckets       = new_num_buckets;
        new_ep.num_data_entries  = new_data_entries;
        new_ep.packed_array_offset    = packed_off;
        new_ep.ludo_buckets_offset    = ludo_off;
        new_ep.lock_array_offset      = lock_off;
        new_ep.global_depth      = old_ep.global_depth + 1;
        new_ep.directory_size    = old_ep.directory_size * 2;
        new_ep.directory_version = old_ep.directory_version + 1;
        new_ep.clients_pending_ack.store(
            reg_->registered_clients.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        new_ep.active_readers.store(0, std::memory_order_relaxed);
        new_ep.active_writers.store(0, std::memory_order_relaxed);
        new_ep.cleanup_allowed.store(0, std::memory_order_relaxed);

        // Reset allocation lanes for new region
        uint32_t effective_lanes = reg_->mem_threads ? reg_->mem_threads : 1;
        for (uint32_t i = 0; i < effective_lanes; ++i) {
            reg_->lane_next_free_index[i] = old_ep.num_data_entries + i;
        }

        // Release fence before publishing version pointer
        std::atomic_thread_fence(std::memory_order_release);
        new_ep.published.store(1, std::memory_order_release);

        reg_->previous_version.store(old_ver, std::memory_order_relaxed);
        reg_->current_version.store(new_ver,  std::memory_order_release);
        reg_->generation.fetch_add(1,          std::memory_order_release);

        set_state(ResizeState::READY_TO_SWITCH);

        metrics_.copy_duration_ms += now_ms() - t0;
        metrics_.resize_count++;
        pending_old_ver_  = old_ver;
        pending_old_base_ = srv_state_->shared_region_base;
        pending_old_fd_   = srv_state_->shared_region_fd;
        pending_old_size_ = old_ep.region_size;

        // Update server state to point to new region
        srv_state_->shared_region_base = new_base;
        srv_state_->shared_region_fd   = new_fd;
        srv_state_->num_buckets        = new_num_buckets;
        srv_state_->num_data_entries   = new_data_entries;
        // lock_array is now in new region
        srv_state_->lock_array = reinterpret_cast<volatile uint8_t*>(new_base_ptr + lock_off);

        gc_start_time_ = 0; // reset

        // Build typed wrappers for the new region so drain_old() can replay
        // deferred inserts/deletes directly into the new data structures.
        char* new_base_ptr2 = reinterpret_cast<char*>(new_base);
        auto* new_ludo_mem  = reinterpret_cast<LudoBucket*>(new_base_ptr2 + ludo_off);
        // Delete previous wrappers if any (shouldn't happen, but be safe)
        delete new_ludo_buckets_;
        delete new_packed_data_;
        new_ludo_buckets_ = new ludo_buckets_t(new_ludo_mem, new_num_buckets);
        new_packed_data_  = new packed_data_t(new_base_ptr2 + packed_off, new_data_entries);

        // Also update server state typed pointers so in-process clients
        // (connected via NumaServerManager) pick up the new region.
        srv_state_->ludo_buckets_ptr = new_ludo_buckets_;
        srv_state_->packed_data_ptr  = new_packed_data_;
    }

    // ── READY_TO_SWITCH: wait for client acks ─────────────────────────────────

    void wait_for_acks_or_timeout() {
        uint64_t cur_ver  = reg_->current_version.load(std::memory_order_acquire);
        auto&    ep       = reg_->epoch[cur_ver % kMaxEpochSlots];

        // Check heartbeats; discount zombies
        uint64_t now = now_ns();
        uint64_t pending = ep.clients_pending_ack.load(std::memory_order_acquire);

        if (pending == 0) {
            set_state(ResizeState::DRAIN_OLD);
            return;
        }

        // Scan client table for zombies and subtract them
        uint64_t zombies = 0;
        for (size_t i = 0; i < kMaxClients; ++i) {
            if (!reg_->clients[i].active.load(std::memory_order_relaxed)) continue;
            uint64_t hb = reg_->clients[i].last_heartbeat_ns.load(std::memory_order_relaxed);
            if (now - hb > kZombieThresholdNs) {
                zombies++;
                metrics_.zombie_cleanups++;
            }
        }

        // If all remaining un-acked clients are zombies
        if (pending <= zombies) {
            ep.clients_pending_ack.store(0, std::memory_order_release);
            set_state(ResizeState::DRAIN_OLD);
            return;
        }

        // Grace window: force-continue after kAckGraceMs
        if (ack_wait_start_ == 0) ack_wait_start_ = now_ms();
        uint64_t elapsed = now_ms() - ack_wait_start_;
        if (elapsed > kAckGraceMs) {
            metrics_.ack_wait_ms += elapsed;
            ep.clients_pending_ack.store(0, std::memory_order_release);
            set_state(ResizeState::DRAIN_OLD);
            ack_wait_start_ = 0;
        }
    }

    // ── DRAIN_OLD: replay deferred mutations, drain readers ──────────────────

    void drain_old() {
        auto t0 = now_ms();

        // Wait for old region active_writers to drain
        if (pending_old_ver_ > 0) {
            auto& old_ep = reg_->epoch[pending_old_ver_ % kMaxEpochSlots];
            if (old_ep.active_writers.load(std::memory_order_acquire) > 0) return;
        }

        // Replay deferred inserts into new region
        {
            std::lock_guard<std::mutex> lk(defer_mu_);

            while (!deferred_inserts_.empty()) {
                const auto& di = deferred_inserts_.front();
                replay_insert(di.key, di.value);
                deferred_inserts_.pop_front();
                reg_->deferred_insert_count.fetch_sub(1, std::memory_order_relaxed);
            }

            while (!deferred_deletes_.empty()) {
                const auto& dd = deferred_deletes_.front();
                replay_delete(dd.key);
                deferred_deletes_.pop_front();
                reg_->deferred_delete_count.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        if (pending_old_ver_ > 0) {
            auto& old_ep = reg_->epoch[pending_old_ver_ % kMaxEpochSlots];
            old_ep.cleanup_allowed.store(1, std::memory_order_release);
        }

        metrics_.drain_duration_ms += now_ms() - t0;
        gc_start_time_ = now_ms();
        set_state(ResizeState::GC_PENDING);
    }

    // ── Replay helpers ────────────────────────────────────────────────────────
    //
    // Mirror the logic of outback_put_direct / outback_remove_direct but
    // operate on new_ludo_buckets_ and new_packed_data_ instead of the
    // server-wide globals.  Called single-threaded from drain_old() under
    // defer_mu_, so no extra locking on the deque is needed; the per-bucket
    // mutex from mutex_array_ still protects the shared data structures.

    void replay_insert(const KeyType& key, const ValType& val) {
        if (!ludo_lookup_ || !new_ludo_buckets_ || !new_packed_data_) return;

        auto loc = ludo_lookup_->lookup_slot(key);
        size_t row = loc.first;

        std::unique_lock<std::mutex> lock(mutex_array_[row]);

        FastHasher64<KeyType> h;
        h.setSeed(ludo_lookup_->buckets[row].seed);
        uint8_t slot = uint8_t(h(key) >> 62);

        int8_t status = new_ludo_buckets_->check_slots(key, row, slot);
        switch (status) {
            case 0: {
                // Empty primary slot — bulk-insert into new packed array
                uint32_t addr = new_packed_data_->bulk_load_data(key, sizeof(ValType), val);
                new_ludo_buckets_->write_addr(key, row, slot, addr);
                break;
            }
            case 1: {
                // Fingerprint match — update value at existing address
                auto addr = new_ludo_buckets_->read_addr(row, slot);
                KeyType stored_key;
                new_packed_data_->read_key(addr, stored_key);
                if (stored_key == key) {
                    new_packed_data_->update_data(addr, key, sizeof(ValType), val);
                }
                break;
            }
            case 2: {
                // Primary slot taken but another slot is free
                for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
                    if (new_ludo_buckets_->check_slots(key, row, s) == 0) {
                        uint32_t addr = new_packed_data_->bulk_load_data(key, sizeof(ValType), val);
                        new_ludo_buckets_->write_addr(key, row, s, addr);
                        break;
                    }
                }
                break;
            }
            case 3:
            default:
                // Bucket full — discard (out-of-band overflow was already in LRU
                // which is not copied; treat as best-effort during resize).
                break;
        }
    }

    void replay_delete(const KeyType& key) {
        if (!ludo_lookup_ || !new_ludo_buckets_ || !new_packed_data_) return;

        auto loc = ludo_lookup_->lookup_slot(key);
        size_t row = loc.first;

        std::unique_lock<std::mutex> lock(mutex_array_[row]);

        // Check primary slot first
        auto addr = new_ludo_buckets_->read_addr(row, loc.second);
        if (new_packed_data_->remove_data_with_key_check(addr, key)) {
            new_ludo_buckets_->remove_addr(row, loc.second);
            return;
        }

        // If primary slot didn't match, scan the rest of the bucket
        for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
            if (s == static_cast<uint8_t>(loc.second)) continue;
            auto a = new_ludo_buckets_->read_addr(row, s);
            if (a == 0) continue;
            if (new_packed_data_->remove_data_with_key_check(a, key)) {
                new_ludo_buckets_->remove_addr(row, s);
                return;
            }
        }
        // Key not found in new region — already absent, nothing to do.
    }

    // ── GC_PENDING: reclaim old region after grace period ────────────────────

    void maybe_gc() {
        if (gc_start_time_ == 0) gc_start_time_ = now_ms();
        uint64_t elapsed = now_ms() - gc_start_time_;
        if (elapsed < kGcGraceMs) return;

        if (pending_old_ver_ > 0) {
            auto& old_ep = reg_->epoch[pending_old_ver_ % kMaxEpochSlots];
            if (old_ep.active_readers.load(std::memory_order_acquire) > 0) return;
        }

        // Unmap and unlink old region
        if (pending_old_base_ && pending_old_size_ > 0) {
            munmap(pending_old_base_, pending_old_size_);
            pending_old_base_ = nullptr;
        }
        if (pending_old_fd_ >= 0) {
            close(pending_old_fd_);
            pending_old_fd_ = -1;
        }
        if (pending_old_ver_ > 0) {
            unlink_numa_region(server_name_, pending_old_ver_);
        }

        pending_old_ver_  = 0;
        pending_old_size_ = 0;

        reg_->previous_version.store(0, std::memory_order_relaxed);
        reg_->generation.fetch_add(1, std::memory_order_release);

        metrics_.gc_delay_ms += now_ms() - gc_start_time_;
        gc_start_time_ = 0;

        set_state(ResizeState::NORMAL);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    void set_state(ResizeState s) {
        // Block structural mutations during COPYING, READY_TO_SWITCH, and DRAIN_OLD
        bool blocked = (s == ResizeState::COPYING ||
                        s == ResizeState::READY_TO_SWITCH ||
                        s == ResizeState::DRAIN_OLD);
        xstore::transport::g_structural_mutations_blocked.store(blocked,
            std::memory_order_release);

        reg_->resize_state.store(static_cast<uint32_t>(s), std::memory_order_release);
        reg_->generation.fetch_add(1, std::memory_order_release);
    }

    bool memory_pressure_ok(size_t current_region_size) const {
        // Heuristic: need at least 2× current region free on the NUMA node
        // We use the system-wide free memory as a conservative proxy.
        struct sysinfo si;
        if (sysinfo(&si) != 0) return true; // if unavailable, allow
        size_t free_bytes = si.freeram * si.mem_unit;
        return free_bytes >= current_region_size * 2;
    }

    static uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }
    static uint64_t now_ms() { return now_ns() / 1'000'000ULL; }

    // ── Members ───────────────────────────────────────────────────────────────

    SharedNumaRegistry* reg_;
    ServerNumaState*    srv_state_;
    std::string         server_name_;
    int                 numa_node_;

    // Lookup table (seeds) — shared with server; does not change during copy
    ludo_lookup_t* ludo_lookup_  = nullptr;
    // Per-bucket mutex array — same array used by op handlers
    std::mutex*    mutex_array_  = nullptr;

    // New-region typed wrappers created in do_copy(), used by replay helpers
    ludo_buckets_t* new_ludo_buckets_ = nullptr;
    packed_data_t*  new_packed_data_  = nullptr;

    std::atomic<bool>   running_{false};
    std::thread         thread_;

    // Deferred mutation queues
    std::mutex defer_mu_;
    std::deque<DeferredInsert<KeyType, ValType>> deferred_inserts_;
    std::deque<DeferredDelete<KeyType>>          deferred_deletes_;

    // Tracking old region during drain / GC
    uint64_t pending_old_ver_  = 0;
    void*    pending_old_base_ = nullptr;
    int      pending_old_fd_   = -1;
    size_t   pending_old_size_ = 0;

    // New region handle (transferred to srv_state_ after publish)
    void* new_region_base_ = nullptr;
    int   new_region_fd_   = -1;

    // Timers
    uint64_t ack_wait_start_ = 0;
    uint64_t gc_start_time_  = 0;

    ResizeMetrics metrics_;
};

} // namespace outback
