#include <random>
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

auto setup_ludo_table() -> bool;

namespace outback {

void run_benchmark(size_t sec);
void* numa_client_worker(void* param);

}

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Initialize NUMA
    init_numa();
    LOG(2) << "[NUMA initialized] Current node: " << get_current_numa_node();

    LOG(2) << "[Loading data] ...";
    bench::load_benchmark_config();
    bench::load_data();

    LOG(2) << "[Setup Ludo table] ...";
    setup_ludo_table();

    LOG(2) << "[Connect to NUMA server] ...";
    if (!connect_to_numa_server("default")) {
        LOG(4) << "Failed to connect to NUMA server";
        return -1;
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
        LOG(2) << "[micro] >>> sec " << current_sec << " throughput: " << tput 
               << ", latency: " << tlat/tput << "us";
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
}

void* numa_client_worker(void* param) {
    thread_param_t &thread_param = *(thread_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;
    
    // Bind thread to local NUMA node (CPU node)
    // bind_to_numa_node(0);  // Uncomment if needed
    
    LOG(2) << "[Worker " << thread_id << "] Starting on NUMA node " << get_current_numa_node();

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
            std::chrono::microseconds duration(0);
            double d = ratio_dis(gen);
            
            if(d <= BenConfig.read_ratio) {             // search
                KeyType dummy_key = bench_keys[query_i];
                auto start_time = std::chrono::high_resolution_clock::now();    
                auto res = numa_search(dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                query_i++;
                if (unlikely(query_i == bench_keys.size())) {
                    query_i = 0;
                }
            } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio) {      // insert
                KeyType dummy_key = nonexist_keys[insert_i];
                auto start_time = std::chrono::high_resolution_clock::now(); 
                numa_put(dummy_key, dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                insert_i++;
                if (unlikely(insert_i == nonexist_keys.size())) {
                    insert_i = 0;
                }
            } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio + BenConfig.update_ratio) { // update
                KeyType dummy_key = bench_keys[update_i];
                auto start_time = std::chrono::high_resolution_clock::now(); 
                numa_update(dummy_key, dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                update_i++;
                if (unlikely(update_i == bench_keys.size())) {
                    update_i = 0;
                }
            } else {  // delete
                KeyType dummy_key = bench_keys[remove_i];
                auto start_time = std::chrono::high_resolution_clock::now();
                numa_remove(dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                remove_i++;
                if (unlikely(remove_i == bench_keys.size())) {
                    remove_i = 0;
                }
            }
            thread_param.throughput++;
            thread_param.latency += static_cast<double>(duration.count());
        }
    } else {  // YCSB workload
        while(running) {
            std::chrono::microseconds duration(0);
            double d = ratio_dis(gen);
            
            if(d <= BenConfig.read_ratio) {             // search
                KeyType dummy_key = bench_keys[query_i];
                auto start_time = std::chrono::high_resolution_clock::now();    
                auto res = numa_search(dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                query_i++;
                if (unlikely(query_i == bench_keys.size())) {
                    query_i = 0;
                }
            } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio) {      // insert
                KeyType dummy_key = std::stoull(workload.NextSequenceKey().substr(4));
                auto start_time = std::chrono::high_resolution_clock::now(); 
                numa_put(dummy_key, dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                insert_i++;
                if (unlikely(insert_i == nonexist_keys.size())) {
                    insert_i = 0;
                }
            } else if(d <= BenConfig.read_ratio + BenConfig.insert_ratio + BenConfig.update_ratio) { // update
                KeyType dummy_key = bench_keys[update_i];
                auto start_time = std::chrono::high_resolution_clock::now(); 
                numa_update(dummy_key, dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                update_i++;
                if (unlikely(update_i == bench_keys.size())) {
                    update_i = 0;
                }
            } else {  // delete
                KeyType dummy_key = bench_keys[remove_i];
                auto start_time = std::chrono::high_resolution_clock::now();
                numa_remove(dummy_key);
                auto end_time = std::chrono::high_resolution_clock::now();
                duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                remove_i++;
                if (unlikely(remove_i == bench_keys.size())) {
                    remove_i = 0;
                }
            }
            thread_param.throughput++;
            thread_param.latency += static_cast<double>(duration.count());
        }
    }
    
    pthread_exit(nullptr);
}

} // namespace outback
