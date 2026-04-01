#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#include <gflags/gflags.h>

#include "r2/src/thread.hh"                   /// Thread
#include "xutils/numa_memory.hh"              /// NUMA memory

#include "outback/outback_server_numa.hh"
#include "outback/outback_resize.hh"
#include "benchs/load_config.hh"
#include "benchs/load_data.hh"

using namespace xstore::numa;
using namespace xstore::transport;
using namespace outback;

using XThread = ::r2::Thread<usize>;

DEFINE_int32(numa_node, 1, "NUMA node to allocate server memory");
DEFINE_double(s_slow_pct, 0.80, "Fraction of packed_entries that triggers PRE_RESIZE (0.0-1.0)");
DEFINE_double(s_stop_pct, 0.95, "Fraction of packed_entries that triggers hard COPYING (0.0-1.0)");

#define DEBUG_MODE_CHECK 0

auto setup_ludo_table() -> bool;
void register_numa_server();
void cleanup_numa_ipc();
void install_signal_handlers();

std::string g_server_name = "default";
std::atomic<bool> g_cleanup_done(false);
volatile sig_atomic_t g_stop_requested = 0;
size_t g_shared_region_size = 0;
std::unique_ptr<outback::ResizeOrchestrator> g_resize_orchestrator;

void signal_handler(int) {
    g_stop_requested = 1;
    running = false;
}

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void cleanup_numa_ipc() {
    if (g_cleanup_done.exchange(true)) {
        return;
    }

    if (g_resize_orchestrator) {
        g_resize_orchestrator->stop();
        g_resize_orchestrator.reset();
    }

    if (server_state && server_state->shared_meta) {
        server_state->shared_meta->ready = 0;
        msync(server_state->shared_meta, sizeof(SharedNumaRegistry), MS_SYNC);
    }

    if (server_state && server_state->shared_meta) {
        munmap(server_state->shared_meta, sizeof(SharedNumaRegistry));
        server_state->shared_meta = nullptr;
    }

    if (server_state && server_state->shared_region_base && g_shared_region_size > 0) {
        munmap(server_state->shared_region_base, g_shared_region_size);
        server_state->shared_region_base = nullptr;
    }

    if (server_state && server_state->shared_meta_fd >= 0) {
        close(server_state->shared_meta_fd);
        server_state->shared_meta_fd = -1;
    }

    if (server_state && server_state->shared_region_fd >= 0) {
        close(server_state->shared_region_fd);
        server_state->shared_region_fd = -1;
    }

    shm_unlink(numa_meta_name(g_server_name).c_str());
    shm_unlink(numa_region_name(g_server_name).c_str());
}

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    g_server_name = make_numa_server_name(bench::FLAGS_server_addr);
    install_signal_handlers();

    // Clean up any stale IPC states before starting
    shm_unlink(numa_meta_name(g_server_name).c_str());
    shm_unlink(numa_region_name(g_server_name, 1).c_str());

    // Initialize NUMA
    init_numa();
    LOG(2) << "[NUMA initialized] Current node: " << get_current_numa_node();
    LOG(2) << "[Server will use NUMA node] " << FLAGS_numa_node;
    LOG(2) << "[NUMA mode] --nic_idx=" << bench::FLAGS_nic_idx << " is accepted for CLI compatibility (not used).";

    // Setup the value
    LOG(2) << "[Outback server loading data] ...";
    bench::load_benchmark_config();
    bench::load_data();

    LOG(2) << "[Server setup ludo table on NUMA node] ...";
    if (!setup_ludo_table()) {
        cleanup_numa_ipc();
        return g_stop_requested ? 130 : -1;
    }

    LOG(2) << "[Register NUMA server state] ...";
    register_numa_server();

    running = true;

    size_t current_sec = 0;
    while (current_sec < bench::FLAGS_seconds + 10 && !g_stop_requested) {
        sleep(1);
        ++current_sec;
    }

    running = false;
    cleanup_numa_ipc();

    LOG(4) << "NUMA server finishes";
    return 0;
}

auto setup_ludo_table() -> bool {
    const size_t packed_entries = 2 * bench::FLAGS_nkeys;

    // Build control-plane lookup first so we can derive final bucket count
    ludo_maintenance_t ludo_maintenance_unit(1024);
    for (uint64_t i = 0; i < bench::FLAGS_nkeys; i++) {
        if (g_stop_requested) {
            return false;
        }
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
    g_shared_region_size = total_size;

    SharedNumaRegistry* shared_meta = nullptr;
    int shared_meta_fd = -1;
    if (!map_numa_registry(g_server_name, true, &shared_meta, &shared_meta_fd)) {
        LOG(4) << "Failed to create/open NUMA shared registry";
        return false;
    }

    void* shared_region_base = nullptr;
    int shared_region_fd = -1;
    if (!create_shared_numa_region(g_server_name, total_size, FLAGS_numa_node,
                                   /*version=*/1, &shared_region_base, &shared_region_fd)) {
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
        if (g_stop_requested) {
            return false;
        }
        KeyType key = bench::exist_keys[i];
        packed_data->bulk_load_data(key, sizeof(V), i);
        r2::compile_fence();
    }

    delete generated_ludo_buckets;

    // Allocate per-bucket mutex array (required by outback_put_direct / replay logic)
    mutexArray = new std::mutex[num_buckets];

    // Allocate LRU cache (local)
    lru_cache = new lru_cache_t(2048, 10);

    // Publish shared metadata
    // protocol_version, current_version, resize_state were set in map_numa_registry()
    shared_meta->magic     = kNumaMetaMagic;
    shared_meta->numa_node = FLAGS_numa_node;
    shared_meta->ready     = 0;

    // Populate epoch slot 1 (version 1 = first data region)
    auto& ep1 = shared_meta->epoch[1];
    ep1.version             = 1;
    ep1.publish_ts_ns       = 0;
    ep1.region_size         = total_size;
    ep1.num_buckets         = num_buckets;
    ep1.num_data_entries    = packed_entries;
    ep1.packed_array_offset = packed_offset;
    ep1.ludo_buckets_offset = ludo_offset;
    ep1.lock_array_offset   = lock_offset;
    ep1.global_depth        = 1;
    ep1.directory_size      = 0;
    ep1.directory_version   = 1;
    ep1.clients_pending_ack.store(0, std::memory_order_relaxed);
    ep1.active_readers.store(0,      std::memory_order_relaxed);
    ep1.active_writers.store(0,      std::memory_order_relaxed);
    ep1.published.store(1,           std::memory_order_release);
    ep1.cleanup_allowed.store(0,     std::memory_order_relaxed);

    // Configure resize thresholds from flags (clamped to sane range)
    const double slow_pct = std::max(0.10, std::min(0.99, FLAGS_s_slow_pct));
    const double stop_pct = std::max(slow_pct + 0.01, std::min(1.00, FLAGS_s_stop_pct));
    shared_meta->s_slow = static_cast<uint64_t>(packed_entries * slow_pct);
    shared_meta->s_stop = static_cast<uint64_t>(packed_entries * stop_pct);

    const size_t effective_mem_threads = std::max<size_t>(1, std::min<size_t>(bench::FLAGS_mem_threads, kMaxNumaLanes));
    shared_meta->mem_threads = static_cast<uint32_t>(effective_mem_threads);
    for (size_t lane = 0; lane < kMaxNumaLanes; ++lane) {
        if (lane < effective_mem_threads) {
            shared_meta->lane_next_free_index[lane] = bench::FLAGS_nkeys + lane;
        } else {
            shared_meta->lane_next_free_index[lane] = packed_entries;
        }
    }
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
    NumaServerManager::instance().register_server_state(g_server_name, server_state);
    NumaServerManager::instance().register_server_state("default", server_state);

    const u64 lane_count = (server_state->shared_meta && server_state->shared_meta->mem_threads > 0)
        ? static_cast<u64>(server_state->shared_meta->mem_threads)
        : 1;
    for (u64 i = 0; i < lane_count; ++i) {
        NumaServerManager::instance().register_server_state("b" + std::to_string(i), server_state);
    }

    if (server_state->shared_meta) {
        server_state->shared_meta->ready = 1;
        msync(server_state->shared_meta, sizeof(SharedNumaRegistry), MS_SYNC);
    }
    
    const auto& cur_ep = current_epoch(*server_state->shared_meta);
    LOG(2) << "Server state registered:";
    LOG(2) << "  - NUMA node: " << server_state->numa_node;
    LOG(2) << "  - ludo_buckets at: " << server_state->ludo_buckets_ptr;
    LOG(2) << "  - packed_data at: " << server_state->packed_data_ptr;
    LOG(2) << "  - num_buckets: " << cur_ep.num_buckets;
    LOG(2) << "  - num_data_entries: " << cur_ep.num_data_entries;
    LOG(2) << "  - mem_threads(lanes): " << static_cast<u64>(server_state->shared_meta ? server_state->shared_meta->mem_threads : 1);
    LOG(2) << "  - server name: " << g_server_name;
    LOG(2) << "  - shared meta: " << numa_meta_name(g_server_name);
    LOG(2) << "  - shared region v1: " << numa_region_name(g_server_name, 1);

    // Start resize orchestrator background thread
    g_resize_orchestrator = std::make_unique<outback::ResizeOrchestrator>(
        server_state->shared_meta, server_state, g_server_name, FLAGS_numa_node,
        ludo_lookup_unit, mutexArray);
    g_resize_orchestrator->start();
    LOG(2) << "  - resize orchestrator started";
}
