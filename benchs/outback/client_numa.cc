#include <random>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <functional>
#include <iomanip>
#include <gflags/gflags.h>

#include "outback/outback_client_numa.hh"

#include "benchs/load_config.hh"
#include "benchs/load_data.hh"
#include "benchs/rolex_util_back.hh"

using namespace r2;
using namespace xstore::util;
using namespace xstore::transport;
using namespace xstore::numa;
using namespace bench;
using namespace outback;

#define DEBUG_MODE_CHECK 0

volatile bool running;
std::atomic<size_t> ready_threads(0);

constexpr size_t kLatencyHistMaxUs = 10000;
constexpr uint64_t kLatencyHistBinNs = 100;
constexpr size_t kLatencyHistBins = (kLatencyHistMaxUs * 1000) / kLatencyHistBinNs + 1;
std::vector<std::vector<uint64_t>> g_latency_hist;

auto setup_ludo_table() -> bool;
std::string get_numa_server_name_for_client();

namespace outback {

void run_benchmark(size_t sec);
void* numa_client_worker(void* param);

}

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Initialize NUMA
    init_numa();
    LOG(2) << "[NUMA initialized] Current node: " << get_current_numa_node();
    LOG(2) << "[NUMA mode] --nic_idx=" << FLAGS_nic_idx << " is accepted for CLI compatibility (not used).";

    LOG(2) << "[Loading data] ...";
    bench::load_benchmark_config();
    bench::load_data();

    LOG(2) << "[Setup Ludo table] ...";
    setup_ludo_table();

    // ── Compute-node DMPH memory report (matches paper Fig. 16) ──────────────
    if (ludo_lookup_unit) {
        const uint64_t nb        = ludo_lookup_unit->num_buckets_;
        const uint64_t seeds_b   = nb;                           // 1 uint8_t per bucket
        const uint64_t othello_b = ludo_lookup_unit->locator.mem.size() * sizeof(uint64_t);
        const double seeds_mb    = static_cast<double>(seeds_b)   / 1024.0 / 1024.0;
        const double othello_mb  = static_cast<double>(othello_b) / 1024.0 / 1024.0;
        const double total_mb    = seeds_mb + othello_mb;
        LOG(2) << "[memory] Compute-node DMPH:"
               << "  num_buckets=" << nb
               << "  seeds_MB=" << seeds_mb
               << "  othello_MB=" << othello_mb
               << "  total_MB=" << total_mb
               << "  load_factor=" << FLAGS_load_factor
               << "  nkeys=" << FLAGS_nkeys;
    }

    if (FLAGS_memory_only) {
        return 0;
    }

    LOG(2) << "[Connect to NUMA server] ...";
    const std::string server_name = get_numa_server_name_for_client();
    while (!connect_to_numa_server(server_name)) {
        LOG(2) << "Waiting for NUMA server to be ready...";
        sleep(2);
    }

    LOG(2) << "[Run benchmark] ...";
    run_benchmark(FLAGS_seconds);
    
    return 0;
}

auto setup_ludo_table() -> bool {
    ludo_maintenance_t ludo_maintenance_unit(1024);
    for (uint64_t i = 0; i < FLAGS_nkeys; i++) {
        ludo_maintenance_unit.insert(exist_keys[i], i);
    }
    LOG(2) << "Ludo slots warmed up...";

    ludo_lookup_unit = new ludo_lookup_t(ludo_maintenance_unit);
    LOG(2) << "Ludo slots finished up...";

    #if DEBUG_MODE_CHECK
        for (uint64_t i = 0; i < 100; i++) {
            auto res = ludo_lookup_unit->lookup_slot(i);
            if (res != std::pair<uint32_t, uint8_t>{}) {
                LOG(3) << "slot lookup test: " << res.first;
            } else {
                LOG(3) << "slot lookup failed";
                exit(0);
            }
        }
    #endif
    exist_keys.clear();
    return true;
}

std::string get_numa_server_name_for_client() {
    std::string base = make_numa_server_name(FLAGS_server_addr);
    if (FLAGS_mem_threads > 0) {
        const auto worker_id = static_cast<u64>(std::max(0, FLAGS_start_threads));
        const auto lane = worker_id % FLAGS_mem_threads;
        LOG(2) << "[NUMA connect lane] b" << lane << " (compat), endpoint: " << base;
    }
    return base;
}

namespace outback {

void run_benchmark(size_t sec) {
    pthread_t threads[BenConfig.threads];
    thread_param_t thread_params[BenConfig.threads];
    
    // Check if parameters are cacheline aligned
    for (size_t i = 0; i < BenConfig.threads; i++) {
        ASSERT((uint64_t)(&(thread_params[i])) % CACHELINE_SIZE == 0) <<
            "wrong parameter address: " << &(thread_params[i]);
    }

    running = false;
    g_latency_hist.assign(BenConfig.threads, std::vector<uint64_t>(kLatencyHistBins, 0));

    for(size_t worker_i = 0; worker_i < BenConfig.threads; worker_i++){
        thread_params[worker_i].thread_id = worker_i;
        thread_params[worker_i].throughput = 0;
        thread_params[worker_i].latency = 0;
        int ret = pthread_create(&threads[worker_i], nullptr, numa_client_worker,
                                (void *)&thread_params[worker_i]);
        ASSERT(ret == 0) << "Error:" << ret;
    }

    LOG(2) << "[Wait for workers ready] ...";
    while (ready_threads < BenConfig.threads) sleep(0.3);

    running = true;
    std::vector<size_t> tput_history(BenConfig.threads, 0);
    std::vector<uint64_t> per_sec_tput;
    std::vector<double> per_sec_avg_latency_us;
    per_sec_tput.reserve(sec);
    per_sec_avg_latency_us.reserve(sec);

    uint64_t total_ops = 0;
    double total_latency_us = 0;

    size_t current_sec = 0;
    while (current_sec < sec) {
        sleep(1);
        uint64_t tput = 0;
        double tlat = 0;
        for (size_t i = 0; i < BenConfig.threads; i++) {
            tput += thread_params[i].throughput - tput_history[i];
            tput_history[i] = thread_params[i].throughput;
            tlat += thread_params[i].latency;
            thread_params[i].latency = 0;
        }
             double avg_lat_us = (tput > 0) ? (tlat / static_cast<double>(tput)) : 0.0;
             per_sec_tput.push_back(tput);
             per_sec_avg_latency_us.push_back(avg_lat_us);
             total_ops += tput;
             total_latency_us += tlat;

        LOG(2) << "[micro] >>> sec " << current_sec << " throughput: " << tput 
                 << ", latency: " << avg_lat_us << "us";
        ++current_sec;
    }
    
    running = false;
    void *status;
    for (size_t i = 0; i < BenConfig.threads; i++) {
        int rc = pthread_join(threads[i], &status);
        ASSERT(!rc) << "Error:unable to join," << rc;
    }
    size_t throughput = 0;
    for (auto &p : thread_params) {
        throughput += p.throughput;
    }
    LOG(2) << "[micro] Throughput(op/s): " << throughput / sec;

    if (!per_sec_tput.empty()) {
        const double mean_tput_ops = static_cast<double>(std::accumulate(per_sec_tput.begin(), per_sec_tput.end(), uint64_t(0))) /
                                     static_cast<double>(per_sec_tput.size());
        const uint64_t max_tput_ops = *std::max_element(per_sec_tput.begin(), per_sec_tput.end());
        const uint64_t min_tput_ops = *std::min_element(per_sec_tput.begin(), per_sec_tput.end());

        const double mean_tput_mops = mean_tput_ops / 1e6;
        const double max_tput_mops = static_cast<double>(max_tput_ops) / 1e6;
        const double min_tput_mops = static_cast<double>(min_tput_ops) / 1e6;

        std::vector<uint64_t> merged_hist(kLatencyHistBins, 0);
        for (const auto& thread_hist : g_latency_hist) {
            for (size_t b = 0; b < kLatencyHistBins; ++b) {
                merged_hist[b] += thread_hist[b];
            }
        }

        uint64_t hist_total = std::accumulate(merged_hist.begin(), merged_hist.end(), uint64_t(0));
        long double weighted_latency_us = 0.0;
        for (size_t b = 0; b < merged_hist.size(); ++b) {
            const long double bin_latency_us = static_cast<long double>(b * kLatencyHistBinNs) / 1000.0L;
            weighted_latency_us += static_cast<long double>(merged_hist[b]) * bin_latency_us;
        }
        const double mean_latency_us = (hist_total > 0)
            ? static_cast<double>(weighted_latency_us / static_cast<long double>(hist_total))
            : 0.0;

        auto percentile_from_rank = [&](double p) -> std::pair<double, uint64_t> {
            if (hist_total == 0) {
                return {0.0, 0};
            }

            // 1-based percentile rank: index = ceil(p * N)
            uint64_t rank = static_cast<uint64_t>(std::ceil(p * static_cast<double>(hist_total)));
            if (rank == 0) {
                rank = 1;
            }

            uint64_t cumulative = 0;
            for (size_t b = 0; b < merged_hist.size(); ++b) {
                cumulative += merged_hist[b];
                if (cumulative >= rank) {
                    double latency_us = static_cast<double>(b * kLatencyHistBinNs) / 1000.0;
                    return {latency_us, rank};
                }
            }

            return {static_cast<double>(kLatencyHistMaxUs), rank};
        };

        const auto p95 = percentile_from_rank(0.95);
        const auto p99 = percentile_from_rank(0.99);
        const auto p999 = percentile_from_rank(0.999);

         LOG(2) << std::fixed << std::setprecision(6)
             << "[summary] Mean Throughput(MOPs): " << mean_tput_mops;
         LOG(2) << std::fixed << std::setprecision(6)
             << "[summary] Max Throughput(MOPs): " << max_tput_mops;
         LOG(2) << std::fixed << std::setprecision(6)
             << "[summary] Min Throughput(MOPs): " << min_tput_mops;
        LOG(2) << std::fixed << std::setprecision(8)
             << "[summary] Mean Latency(us): " << mean_latency_us;
         LOG(2) << std::fixed << std::setprecision(8)
             << "[summary] Tail Latency P95(us): " << p95.first;
         LOG(2) << std::fixed << std::setprecision(8)
             << "[summary] Tail Latency P99(us): " << p99.first;
         LOG(2) << std::fixed << std::setprecision(8)
             << "[summary] Tail Latency P999(us): " << p999.first;
         LOG(2) << "[summary] Percentile Samples N: " << hist_total
             << ", ranks(p95/p99/p999): " << p95.second << "/" << p99.second << "/" << p999.second;
    }
}

void* numa_client_worker(void* param) {
    thread_param_t &thread_param = *(thread_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;

    uint32_t effective_lanes = (remote_registry && remote_registry->mem_threads > 0)
        ? std::min<uint32_t>(remote_registry->mem_threads, static_cast<uint32_t>(kMaxNumaLanes))
        : static_cast<uint32_t>(std::max<uint64_t>(uint64_t(1), FLAGS_mem_threads));
    uint32_t lane = (static_cast<uint32_t>(std::max(0, FLAGS_start_threads)) + thread_id) % effective_lanes;
    set_remote_lane_id(lane);
    
    // Bind thread to local NUMA node (CPU node)
    // bind_to_numa_node(0);  // Uncomment if needed
    
        LOG(2) << "[Worker " << thread_id << "] Starting on NUMA node " << get_current_numa_node()
            << ", lane b" << lane;

    // Generate test data indices
    size_t query_i = 0, insert_i = 0, remove_i = 0, update_i = 0;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> ratio_dis(0, 1);

    LOG(2) << "[micro] Worker: " << thread_id << " Ready.";
    ready_threads++;

    while (!running);

    /**
     * @brief Main benchmark loop - direct NUMA memory access
     */
    if(bench::BenConfig.workloads >= NORMAL) {
        while(running) {
            const int coro_factor = std::max(1, BenConfig.coros);
            for (int coro_i = 0; coro_i < coro_factor && running; ++coro_i) {
                uint64_t duration_ns = 0;
                double d = ratio_dis(gen);

                if(d <= BenConfig.read_ratio) {             // search
                    KeyType dummy_key = bench_keys[query_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    auto res = numa_search(dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    query_i++;
                    if (unlikely(query_i == bench_keys.size())) {
                        query_i = 0;
                    }
                } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio) {      // insert
                    KeyType dummy_key = nonexist_keys[insert_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_put(dummy_key, dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    insert_i++;
                    if (unlikely(insert_i == nonexist_keys.size())) {
                        insert_i = 0;
                    }
                } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio + BenConfig.update_ratio) { // update
                    KeyType dummy_key = bench_keys[update_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_update(dummy_key, dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    update_i++;
                    if (unlikely(update_i == bench_keys.size())) {
                        update_i = 0;
                    }
                } else {  // delete
                    KeyType dummy_key = bench_keys[remove_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_remove(dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    remove_i++;
                    if (unlikely(remove_i == bench_keys.size())) {
                        remove_i = 0;
                    }
                }
                thread_param.throughput++;
                thread_param.latency += static_cast<double>(duration_ns) / 1000.0;
                size_t lat_bucket = static_cast<size_t>(duration_ns / kLatencyHistBinNs);
                if (lat_bucket >= kLatencyHistBins) {
                    lat_bucket = kLatencyHistBins - 1;
                }
                g_latency_hist[thread_id][lat_bucket]++;
            }
        }
    } else {  // YCSB workload
        while(running) {
            const int coro_factor = std::max(1, BenConfig.coros);
            for (int coro_i = 0; coro_i < coro_factor && running; ++coro_i) {
                uint64_t duration_ns = 0;
                double d = ratio_dis(gen);

                if(d <= BenConfig.read_ratio) {             // search
                    KeyType dummy_key = bench_keys[query_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    auto res = numa_search(dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    query_i++;
                    if (unlikely(query_i == bench_keys.size())) {
                        query_i = 0;
                    }
                } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio) {      // insert
                    KeyType dummy_key = std::stoull(workload.NextSequenceKey().substr(4));
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_put(dummy_key, dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    insert_i++;
                    if (unlikely(insert_i == nonexist_keys.size())) {
                        insert_i = 0;
                    }
                } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio + BenConfig.update_ratio) { // update
                    KeyType dummy_key = bench_keys[update_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_update(dummy_key, dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    update_i++;
                    if (unlikely(update_i == bench_keys.size())) {
                        update_i = 0;
                    }
                } else {  // delete
                    KeyType dummy_key = bench_keys[remove_i];
                    auto start_time = std::chrono::high_resolution_clock::now();
                    numa_remove(dummy_key);
                    auto end_time = std::chrono::high_resolution_clock::now();
                    duration_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
                    remove_i++;
                    if (unlikely(remove_i == bench_keys.size())) {
                        remove_i = 0;
                    }
                }
                thread_param.throughput++;
                thread_param.latency += static_cast<double>(duration_ns) / 1000.0;
                size_t lat_bucket = static_cast<size_t>(duration_ns / kLatencyHistBinNs);
                if (lat_bucket >= kLatencyHistBins) {
                    lat_bucket = kLatencyHistBins - 1;
                }
                g_latency_hist[thread_id][lat_bucket]++;
            }
        }
    }
    
    pthread_exit(nullptr);
}

} // namespace outback
