#pragma once

#include "r2/src/thread.hh"                   /// Thread
#include "xutils/local_barrier.hh"            /// PBarrier
#include "xutils/numa_memory.hh"              /// NUMA memory management
#include "xcomm/src/transport/numa_transport.hh" /// NUMA transport
#include "ludo/hashutils/hash.h"

#include "outback/trait_numa.hpp"

using namespace test;
using namespace xstore::transport;
using namespace xstore::numa;
using namespace outback;

/*****  GLOBAL VARIABLES ******/
volatile bool running = true;
std::atomic<size_t> ready_threads(0);
std::mutex* mutexArray;

/*****  LUDO BUCKETS AND UNDERLYING DATA (ON NUMA NODE) *****/
lru_cache_t* lru_cache;
ludo_lookup_t* ludo_lookup_unit;  // act like seeds
outback::ludo_buckets_t* ludo_buckets;
outback::packed_data_t* packed_data;

// NUMA memory region for server data
std::shared_ptr<NumaRegion> numa_mem_region;
ServerNumaState* server_state;

namespace outback {

using XThread = ::r2::Thread<usize>;   // <usize> represents the return type of a function

// Direct function callbacks (no RPC needed)
void outback_get_direct(const size_t loc, const KeyType* key_ptr, ReplyValue* reply);
void outback_put_direct(const size_t loc, const KeyType& key, const ValType& val, ReplyValue* reply);
void outback_update_direct(const size_t loc, const KeyType& key, const ValType& val, ReplyValue* reply);
void outback_remove_direct(const size_t loc, const KeyType& key, ReplyValue* reply);
void outback_scan_direct(const KeyType& key, const u64& n, std::vector<V>* result);

// GET - Direct memory access version
void outback_get_direct(const size_t loc, const KeyType* key_ptr, ReplyValue* reply) {
    ValType dummy_value;
    auto addr = ludo_buckets->read_addr(loc/SLOTS_NUM_BUCKET, loc%SLOTS_NUM_BUCKET);
    dummy_value = packed_data->rawArray[addr].data;
    
    if (true) { // Always success in direct case
        reply->status = true;
        reply->val = dummy_value;
    } else {
        KeyType key = (key_ptr) ? *key_ptr : 0;
        auto cache_bit = ludo_buckets->read_cachebit(loc/SLOTS_NUM_BUCKET, loc%SLOTS_NUM_BUCKET);
        if (cache_bit && lru_cache) {
            dummy_value = lru_cache->get(key);
        }
        reply->status = false;
        reply->val = dummy_value;
    }
}

// PUT - Direct memory access version
void outback_put_direct(const size_t loc, const KeyType& key, const ValType& val, ReplyValue* reply) {
    // If a resize is in progress and structural mutations are blocked,
    // enqueue the insert for later replay rather than writing to the old region.
    if (xstore::transport::g_structural_mutations_blocked.load(
            std::memory_order_acquire)) {
        if (xstore::transport::g_resize_orch_ptr &&
            xstore::transport::g_defer_insert_fn) {
            xstore::transport::g_defer_insert_fn(
                xstore::transport::g_resize_orch_ptr,
                static_cast<uint64_t>(key),
                static_cast<uint64_t>(val));
        }
        reply->status = false;
        reply->val    = static_cast<uint64_t>(xstore::transport::OpStatus::DEFERRED);
        return;
    }

    FastHasher64<K> h;
    
    size_t row = loc / SLOTS_NUM_BUCKET;
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    h.setSeed(ludo_lookup_unit->buckets[row].seed);
    uint8_t slot = uint8_t(h(key) >> 62);
    
    int8_t status = ludo_buckets->check_slots(key, row, slot);
    switch (status) {
        case 0: {
            // Empty slot - insert new entry
            auto addr = packed_data->bulk_load_data(key, sizeof(ValType), val);
            ludo_buckets->write_addr(key, row, slot, addr);
            reply->status = true;
            reply->val = 0;
            break;
        }
        case 1: {
            // Update existing entry
            auto addr = ludo_buckets->read_addr(row, slot);
            KeyType key_;
            packed_data->read_key(addr, key_);
            if (likely(key_ == key)) {
                packed_data->update_data(addr, key, 64, val);
                reply->status = true;
                reply->val = 1;
            } else {
                reply->status = false;
                reply->val = 0;
            }
            break;
        }
        case 2: {
            // Bucket has empty slots
            for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
                if (ludo_buckets->check_slots(key, row, s) == 0) {
                    auto addr = packed_data->bulk_load_data(key, sizeof(ValType), val);
                    ludo_buckets->write_addr(key, row, s, addr);
                    reply->status = true;
                    reply->val = 2;
                    break;
                }
            }
            break;
        }
        case 3: {
            // Bucket is full - need to update seed or handle overflow
            // For now, cache it
            if (lru_cache) {
                lru_cache->put(key, val);
                ludo_buckets->set_cachebit(row, slot);
            }
            reply->status = true;
            reply->val = (1UL << 8) | ludo_lookup_unit->buckets[row].seed;
            break;
        }
        default:
            reply->status = false;
            reply->val = 0;
            break;
    }
}

// UPDATE - Direct memory access version
void outback_update_direct(const size_t loc, const KeyType& key, const ValType& val, ReplyValue* reply) {
    size_t row = loc / SLOTS_NUM_BUCKET;
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    
    auto addr = ludo_buckets->read_addr(row, loc % SLOTS_NUM_BUCKET);
    KeyType key_;
    packed_data->read_key(addr, key_);
    
    if (likely(key_ == key)) {
        packed_data->update_data(addr, key, 64, val);
        reply->status = true;
        reply->val = 1;
    } else {
        reply->status = false;
        reply->val = 0;
    }
}

// REMOVE - Direct memory access version
void outback_remove_direct(const size_t loc, const KeyType& key, ReplyValue* reply) {
    // If a resize is in progress and structural mutations are blocked,
    // enqueue the delete for later replay rather than modifying the old region.
    if (xstore::transport::g_structural_mutations_blocked.load(
            std::memory_order_acquire)) {
        if (xstore::transport::g_resize_orch_ptr &&
            xstore::transport::g_defer_delete_fn) {
            xstore::transport::g_defer_delete_fn(
                xstore::transport::g_resize_orch_ptr,
                static_cast<uint64_t>(key));
        }
        reply->status = false;
        reply->val    = static_cast<uint64_t>(xstore::transport::OpStatus::DEFERRED);
        return;
    }

    size_t row = loc / SLOTS_NUM_BUCKET;
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    
    auto addr = ludo_buckets->read_addr(row, loc % SLOTS_NUM_BUCKET);
    auto res = packed_data->remove_data_with_key_check(addr, key);
    if (res) {
        ludo_buckets->remove_addr(row, loc % SLOTS_NUM_BUCKET);
    }
    
    reply->status = true;
    reply->val = 0;
}

// SCAN - Direct memory access version
void outback_scan_direct(const KeyType& key, const u64& n, std::vector<V>* result) {
    // Simplified scan implementation
    // In a real implementation, this would iterate through the data structure
    result->clear();
    // For now, just return empty result
}

} // namespace outback
