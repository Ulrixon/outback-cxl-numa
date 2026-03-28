#include <iostream>
#include <stdexcept>
#include <gflags/gflags.h>

#include "r2/src/thread.hh"                   /// Thread
#include "xutils/numa_memory.hh"              /// NUMA memory

#include "outback/outback_server_numa.hh"
#include "benchs/load_config.hh"
#include "benchs/load_data.hh"

using namespace xstore::numa;
using namespace xstore::transport;
using namespace outback;

using XThread = ::r2::Thread<usize>;

DEFINE_int32(numa_node, 1, "NUMA node to allocate server memory");

#define DEBUG_MODE_CHECK 0

auto setup_ludo_table() -> bool;
void register_numa_server();

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    // Initialize NUMA
    init_numa();
    LOG(2) << "[NUMA initialized] Current node: " << get_current_numa_node();
    LOG(2) << "[Server will use NUMA node] " << FLAGS_numa_node;

    // Setup the value
    LOG(2) << "[Outback server loading data] ...";
    bench::load_benchmark_config();
    bench::load_data();

    LOG(2) << "[Server setup ludo table on NUMA node] ...";
    setup_ludo_table();

    LOG(2) << "[Register NUMA server state] ...";
    register_numa_server();

    running = true;

    size_t current_sec = 0;
    while (current_sec < bench::FLAGS_seconds + 10) {
        sleep(1);
        ++current_sec;
    }

    running = false;

    LOG(4) << "NUMA server finishes";
    return 0;
}

auto setup_ludo_table() -> bool {
    // Allocate NUMA memory for server data structures
    size_t packed_data_size = sizeof(packed_data_t) + 2 * bench::FLAGS_nkeys * sizeof(packed_struct_t);
    size_t ludo_buckets_size = sizeof(ludo_buckets_t) + 1024 * sizeof(LudoBucket);
    size_t total_size = packed_data_size + ludo_buckets_size + 64 * 1024 * 1024; // Extra space

    numa_mem_region = NumaRegion::create(total_size, FLAGS_numa_node);
    if (!numa_mem_region) {
        LOG(4) << "Failed to allocate NUMA memory";
        return false;
    }

    // Allocate structures on NUMA node
    NumaAllocator allocator(numa_mem_region);
    
    // Allocate packed_data on NUMA node
    void* packed_data_mem = allocator.alloc(packed_data_size);
    packed_data = new (packed_data_mem) packed_data_t(2 * bench::FLAGS_nkeys);
    
    // Allocate LRU cache (can be local or NUMA)
    lru_cache = new lru_cache_t(2048, 10);
    
    // Build ludo maintenance unit
    ludo_maintenance_t ludo_maintenance_unit(1024);
    for (uint64_t i = 0; i < bench::FLAGS_nkeys; i++) {
        KeyType key = bench::exist_keys[i];
        V addr = packed_data->bulk_load_data(key, sizeof(V), i);
        ludo_maintenance_unit.insert(key, addr);
        r2::compile_fence();
    }
    LOG(2) << "Ludo slots warmed up...";

    // Create lookup unit
    ludo_lookup_unit = new ludo_lookup_t(ludo_maintenance_unit);
    ludo_lookup_t ludo_lookup_table(ludo_maintenance_unit, ludo_buckets);
    
    // Allocate mutex array
    mutexArray = new std::mutex[ludo_lookup_unit->getBucketsNum()];
    LOG(2) << "Ludo slots finished up...";

    #if DEBUG_MODE_CHECK
    for (uint64_t i = 0; i < 100; i++) {
        V dummy_value;
        auto loc = ludo_lookup_unit->lookup_slot(i);
        auto addr = ludo_buckets->read_addr(loc.first, loc.second);
        packed_data->read_data(addr, dummy_value);
        LOG(3) << "Verify key " << i << " -> value: " << dummy_value;
    }
    #endif

    bench::exist_keys.clear();
    return true;
}

void register_numa_server() {
    // Create server state structure
    server_state = new ServerNumaState();
    server_state->ludo_buckets_ptr = ludo_buckets;
    server_state->packed_data_ptr = packed_data;
    server_state->num_buckets = ludo_lookup_unit->getBucketsNum();
    server_state->num_data_entries = 2 * bench::FLAGS_nkeys;
    server_state->mutexArray = mutexArray;
    server_state->numa_node = FLAGS_numa_node;

    // Register with global manager
    NumaServerManager::instance().register_server_state("default", server_state);
    
    LOG(2) << "Server state registered:";
    LOG(2) << "  - NUMA node: " << server_state->numa_node;
    LOG(2) << "  - ludo_buckets at: " << server_state->ludo_buckets_ptr;
    LOG(2) << "  - packed_data at: " << server_state->packed_data_ptr;
    LOG(2) << "  - num_buckets: " << server_state->num_buckets;
    LOG(2) << "  - num_data_entries: " << server_state->num_data_entries;
}
