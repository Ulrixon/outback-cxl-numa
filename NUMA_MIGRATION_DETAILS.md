# Outback Architecture Migration: RDMA to NUMA

This document provides a detailed, technical comparison of the mechanisms used in the original RDMA (Remote Direct Memory Access) implementation of Outback versus the newly engineered NUMA (Non-Uniform Memory Access) CXL-emulation layer. 

The findings below are derived from a direct codebase analysis contrasting the original files (`benchs/outback/client.cc`, `benchs/outback/server.cc`, `outback_client.hh`) against the new environment (`benchs/outback/client_numa.cc`, `benchs/outback/server_numa.cc`, `numa_transport.hh`).

---

## 1. The Transport Layer & Server Registration

### Before: RDMA Network Protocol
The original server implementation acts as a network daemon holding local heap memory and sharing it via the Mellanox NIC.
*   **Mechanism:** Uses `UDTransport` (Unreliable Datagram) and registers memory regions (MRs) with hardware. 
*   **Registration:** The server defines remote procedure call callbacks and enters an active polling loop:
    ```cpp
    RPCCore<SendTrait, RecvTrait, SManager> rpc(12);
    rpc.reg_callback(outback_get_callback) == GET;
    rpc.recv_event_loop(&recv);
    ```

### After: POSIX Shared Memory (NUMA)
The new NUMA server acts as a shared-memory allocator, allowing clients to map its memory space directly into their own processes without network overhead.
*   **Mechanism:** Replaces network RPCs with POSIX standard Inter-Process Communication (IPC). Uses `shm_open()` to create file descriptors in `/dev/shm`, sizes them using `ftruncate()`, and physically binds the pages to a specific CPU socket via `init_numa()` and `numactl`.
*   **Registration:** The server maps the data structures (`ludo_buckets`, `packed_data`) into these files and flags `shared_meta->ready = 1`. Clients directly call standard `mmap()` to acquire synchronous pointer access to the server's data.

---

## 2. Execution Model & Coroutines

### Before: Asynchronous Yielding
Because real network round-trips take several microseconds, the CPU cannot afford to block while waiting for RDMA packets.
*   **Mechanism:** Deeply integrates with the `R2` coroutine scheduler. Threads spawn lightweight coroutines to dispatch RDMA VERBS requests and immediately yield execution (`R2_ASYNC_WAIT`) so the thread can process other queries in the meantime.
    ```cpp
    // benchs/outback/client.cc
    ssched.spawn([&rpc, ...](R2_ASYNC) {
        auto res = remote_search(dummy_key, rpc, sender, lkey, R2_ASYNC_WAIT);
    });
    ```

### After: Synchronous Loop Batching
In CXL/NUMA emulation, remote memory access resolves in ~100-300 nanoseconds. Switching coroutine context takes more time than the actual data retrieval.
*   **Mechanism:** All `ssched.spawn` and asynchronous yields were removed. `numa_search` executes as a direct memory pointer read. To support the legacy `--coros` logic, we repurposed the parameter into an inner `for` loop batching factor to maximize cache locality and minimize the overhead of checking the outer `while(running)` state.

---

## 3. Concurrency & Allocators (`--mem_threads`)

### Before: Hardware Queue Pairs
To prevent massive bottlenecks when multiple clients hit the server at once, RDMA utilizes hardware Queue Pairs (QPs) directly on the NIC to multiplex the traffic.

### After: Virtual Software Lanes
Without a NIC, all concurrent `__sync_fetch_and_add` scaling requests from 24+ clients hammer the exact same memory address on the server, causing severe CPU cache-line bouncing (contention).
*   **Mechanism:** To mimic hardware QPs, we built a partitioned allocator within the `SharedNumaRegistry`. Setting `--mem_threads=16` allocates 16 independent atomic counters (`lane_next_free_index[16]`) inside the shared memory. Client threads modulo against this array (`thread_id % mem_threads`) to guarantee parallel, lock-free insertions across the NUMA nodes.

---

## 4. Telemetry and Sub-Microsecond Accuracy

### Before: Basic Timing Aggregations
Network architectures batch reporting using standard chronometrics.
*   **Mechanism:** The clients sum up round-trip latencies per thread (`tlat += thread_params[i].latency;`) and execute a basic division (`tlat / tput`) at the end of every testing second to print a mean average.

### After: Lock-Free Histogram Profiling
Because NUMA latency operates on the magnitude of 100ns, maintaining running totals dynamically throttles the fast path. Furthermore, standard averages disguise high-tail latencies (P99, P999).
*   **Mechanism:** We deployed a massive, lock-free 2D histogram array explicitly sized to the nanosecond boundary (`g_latency_hist[thread_id][kLatencyHistBins]`). 
*   **Execution:** Each thread casts its delta time to an array index and simply increments the bucket integer (`g_latency_hist[thread_id][lat_bucket]++`). Execution throughput is unharmed, and a separate background routine statically parses the arrays during teardown to emit statistically flawless min/max/mean/P99 latency math.

---

## 5. OS Garbage Collection & Teardown

### Before: Ephemeral Process Exits
If the RDMA target is interrupted (`Ctrl+C`), the Linux kernel's standard exit routines cleanly reclaim the raw process memory, close active sockets, and tear down standard memory mappings.

### After: Persistent File Handlers & Signal Catching
Because our NUMA transport utilizes `/dev/shm/*` files, an interrupted backend will permanently anchor gigabytes of test data to the system's active RAM, resulting in system degradation across repeated runs.
*   **Mechanism:** Engineered standard UNIX signal traps (`sigaction(SIGINT)` / `sigaction(SIGTERM)`). When `Ctrl+C` is engaged, it triggers a unified atomic process (`cleanup_numa_ipc()`) to `msync` safely, `munmap` local pointers, and `shm_unlink` the `/dev/shm` registry entries so subsequent runs don't encounter stale `ready = 1` files.

---

## 6. Dynamic Limits vs. Constant Limits (DMPH Load Factor)

### Before: Compile-Time Bounds
To configure the density, collisions, and layout physics of the DMPH Ludo tables, users had to go to the header files natively and recompile.
*   **Mechanism:** `static constexpr double kLoadFactor = 0.95;` inside `deps/ludo/ludo_cp_dp.h`. Modifying this forced complete rebuilds of the server index environment.

### After: Dynamic Runtime Loading
We decoupled the environment bounds to evaluate various storage collision characteristics from the command line on the fly.
*   **Mechanism:** Removed the `constexpr` limits and declared a globally weak symbol: `__attribute__((weak)) double g_ludo_load_factor = 0.95;` in `cuckoo_ht.h`. `bench::load_benchmark_config()` now parses the `--load_factor=0.XX` CLI argument, mutating the physical behavior dynamically upon backend startup without linker penalties.