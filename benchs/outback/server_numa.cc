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

constexpr const char* kDefaultServerName = "default";

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
    const size_t packed_entries = 2 * bench::FLAGS_nkeys;

    // Build control-plane lookup first so we can derive final bucket count
    ludo_maintenance_t ludo_maintenance_unit(1024);
    for (uint64_t i = 0; i < bench::FLAGS_nkeys; i++) {
        KeyType key = bench::exist_keys[i];
        ludo_maintenance_unit.insert(key, i);
        r2::compile_fence();
    }

    ludo_lookup_unit = new ludo_lookup_t(ludo_maintenance_unit);
    ludo_buckets_t* generated_ludo_buckets = nullptr;
    ludo_lookup_t generated_lookup(ludo_maintenance_unit, generated_ludo_buckets);

    size_t num_buckets = ludo_lookup_unit->getBucketsNum();
    size_t packed_array_size = packed_entries * sizeof(packed_struct_t);
    size_t ludo_buckets_array_size = num_buckets * sizeof(LudoBucket);
    size_t lock_array_size = num_buckets * sizeof(uint8_t);

    size_t packed_offset = align_up(0, 64);
    size_t ludo_offset = align_up(packed_offset + packed_array_size, 64);
    size_t lock_offset = align_up(ludo_offset + ludo_buckets_array_size, 64);
    size_t total_size = align_up(lock_offset + lock_array_size, 4096);

    SharedNumaRegistry* shared_meta = nullptr;
    int shared_meta_fd = -1;
    if (!map_numa_registry(kDefaultServerName, true, &shared_meta, &shared_meta_fd)) {
        LOG(4) << "Failed to create/open NUMA shared registry";
        return false;
    }

    void* shared_region_base = nullptr;
    int shared_region_fd = -1;
    if (!create_shared_numa_region(kDefaultServerName, total_size, FLAGS_numa_node,
                                   &shared_region_base, &shared_region_fd)) {
        LOG(4) << "Failed to create/map NUMA shared region";
        return false;
    }

    auto* region_base = reinterpret_cast<char*>(shared_region_base);
    auto* packed_array_mem = region_base + packed_offset;
    auto* ludo_buckets_mem = reinterpret_cast<LudoBucket*>(region_base + ludo_offset);
    lockArray = reinterpret_cast<volatile uint8_t*>(region_base + lock_offset);

    packed_data = new packed_data_t(packed_array_mem, packed_entries);
    ludo_buckets = new ludo_buckets_t(ludo_buckets_mem, num_buckets);

    for (size_t i = 0; i < num_buckets; ++i) {
        ludo_buckets_mem[i] = generated_ludo_buckets->bucketsArray[i];
    }

    for (size_t i = 0; i < lock_array_size; ++i) {
        lockArray[i] = 0;
    }

    // Load packed data into shared mapped region
    for (uint64_t i = 0; i < bench::FLAGS_nkeys; i++) {
        KeyType key = bench::exist_keys[i];
        packed_data->bulk_load_data(key, sizeof(V), i);
        r2::compile_fence();
    }

    delete generated_ludo_buckets;

    // Allocate LRU cache (local)
    lru_cache = new lru_cache_t(2048, 10);

    // Publish shared metadata
    shared_meta->magic = kNumaMetaMagic;
    shared_meta->version = 1;
    shared_meta->ready = 0;
    shared_meta->numa_node = FLAGS_numa_node;
    shared_meta->num_buckets = num_buckets;
    shared_meta->num_data_entries = packed_entries;
    shared_meta->region_size = total_size;
    shared_meta->packed_array_offset = packed_offset;
    shared_meta->ludo_buckets_offset = ludo_offset;
    shared_meta->lock_array_offset = lock_offset;
    shared_meta->next_free_index = bench::FLAGS_nkeys;
    msync(shared_meta, sizeof(SharedNumaRegistry), MS_SYNC);

    server_state = new ServerNumaState();
    server_state->ludo_buckets_ptr = ludo_buckets;
    server_state->packed_data_ptr = packed_data;
    server_state->num_buckets = num_buckets;
    server_state->num_data_entries = packed_entries;
    server_state->lock_array = lockArray;
    server_state->numa_node = FLAGS_numa_node;
    server_state->shared_meta = shared_meta;
    server_state->shared_region_base = shared_region_base;
    server_state->shared_region_fd = shared_region_fd;
    server_state->shared_meta_fd = shared_meta_fd;

    LOG(2) << "Ludo slots warmed up...";
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
    // Register with global manager
    NumaServerManager::instance().register_server_state(kDefaultServerName, server_state);

    if (server_state->shared_meta) {
        server_state->shared_meta->ready = 1;
        msync(server_state->shared_meta, sizeof(SharedNumaRegistry), MS_SYNC);
    }
    
    LOG(2) << "Server state registered:";
    LOG(2) << "  - NUMA node: " << server_state->numa_node;
    LOG(2) << "  - ludo_buckets at: " << server_state->ludo_buckets_ptr;
    LOG(2) << "  - packed_data at: " << server_state->packed_data_ptr;
    LOG(2) << "  - num_buckets: " << server_state->num_buckets;
    LOG(2) << "  - num_data_entries: " << server_state->num_data_entries;
    LOG(2) << "  - shared meta: " << numa_meta_name(kDefaultServerName);
    LOG(2) << "  - shared region: " << numa_region_name(kDefaultServerName);
}
