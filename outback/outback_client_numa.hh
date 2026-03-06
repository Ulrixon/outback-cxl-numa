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

namespace outback {

/**
 * @brief Direct memory access to server's NUMA memory (replaces remote_search via RDMA)
 */
inline auto numa_search(const KeyType& key) -> ::r2::Option<ValType>
{
    if (!remote_server_state) {
        return {};
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    auto num = SLOTS_NUM_BUCKET * loc.first + loc.second;
    
    // Direct access to server's ludo_buckets and packed_data on NUMA node
    auto* ludo_buckets = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
    auto* packed_data = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
    
    auto addr = ludo_buckets->read_addr(loc.first, loc.second);
    ValType value = packed_data->rawArray[addr].data;
    
    return value;
}

/**
 * @brief Direct memory access PUT operation (replaces remote_put via RDMA)
 */
inline void numa_put(const KeyType& key, const ValType& val)
{
    if (!remote_server_state) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    auto num = SLOTS_NUM_BUCKET * loc.first + loc.second;
    size_t row = loc.first;
    
    // Direct access to server structures
    auto* ludo_buckets = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
    auto* packed_data = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
    std::mutex* mutexArray = remote_server_state->mutexArray;
    
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    
    FastHasher64<K> h;
    h.setSeed(ludo_lookup_unit->buckets[row].seed);
    uint8_t slot = uint8_t(h(key) >> 62);
    
    int8_t status = ludo_buckets->check_slots(key, row, slot);
    
    switch (status) {
        case 0: {
            // Empty slot - insert new entry
            auto addr = packed_data->bulk_load_data(key, sizeof(ValType), val);
            ludo_buckets->write_addr(key, row, slot, addr);
            break;
        }
        case 1: {
            // Update existing entry
            auto addr = ludo_buckets->read_addr(row, slot);
            KeyType key_;
            packed_data->read_key(addr, key_);
            if (likely(key_ == key)) {
                packed_data->update_data(addr, key, 64, val);
            }
            break;
        }
        case 2: {
            // Bucket has empty slots
            for (uint8_t s = 0; s < SLOTS_NUM_BUCKET; s++) {
                if (ludo_buckets->check_slots(key, row, s) == 0) {
                    auto addr = packed_data->bulk_load_data(key, sizeof(ValType), val);
                    ludo_buckets->write_addr(key, row, s, addr);
                    break;
                }
            }
            break;
        }
        case 3: {
            // Bucket is full - need to handle overflow
            // For now, just update the first slot
            auto addr = ludo_buckets->read_addr(row, slot);
            packed_data->update_data(addr, key, 64, val);
            // Update seed if needed
            ludo_lookup_unit->updateSeed(row, ludo_lookup_unit->buckets[row].seed + 1);
            break;
        }
    }
}

/**
 * @brief Direct memory access UPDATE operation (replaces remote_update via RDMA)
 */
inline void numa_update(const KeyType& key, const ValType& val)
{
    if (!remote_server_state) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    
    auto* ludo_buckets = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
    auto* packed_data = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
    std::mutex* mutexArray = remote_server_state->mutexArray;
    
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    
    auto addr = ludo_buckets->read_addr(row, loc.second);
    KeyType key_;
    packed_data->read_key(addr, key_);
    
    if (likely(key_ == key)) {
        packed_data->update_data(addr, key, 64, val);
    }
}

/**
 * @brief Direct memory access REMOVE operation (replaces remote_remove via RDMA)
 */
inline void numa_remove(const KeyType& key)
{
    if (!remote_server_state) {
        return;
    }

    auto loc = ludo_lookup_unit->lookup_slot(key);
    size_t row = loc.first;
    
    auto* ludo_buckets = reinterpret_cast<ludo_buckets_t*>(remote_server_state->ludo_buckets_ptr);
    auto* packed_data = reinterpret_cast<packed_data_t*>(remote_server_state->packed_data_ptr);
    std::mutex* mutexArray = remote_server_state->mutexArray;
    
    std::unique_lock<std::mutex> lock(mutexArray[row]);
    
    auto addr = ludo_buckets->read_addr(row, loc.second);
    auto res = packed_data->remove_data_with_key_check(addr, key);
    if (res) {
        ludo_buckets->remove_addr(row, loc.second);
    }
}

/**
 * @brief Direct memory access SCAN operation (replaces remote_scan via RDMA)
 */
inline std::vector<V> numa_scan(const KeyType& key, const u64& n)
{
    std::vector<V> result;
    if (!remote_server_state) {
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
    if (!remote_server_state) {
        LOG(4) << "Failed to connect to NUMA server: " << server_name;
        return false;
    }
    LOG(2) << "Connected to NUMA server: " << server_name 
           << " on node " << remote_server_state->numa_node;
    return true;
}

} // namespace outback
