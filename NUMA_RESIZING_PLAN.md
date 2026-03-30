# NUMA Resizing Architecture & Implementation Plan

## 1. Overview
In a distributed memory environment using RDMA, hash table resizing requires the server to allocate new local memory, register new Memory Regions (MRs) with the NIC, and distribute the new `rkey` and memory addresses to clients. 

In our simulated CXL/NUMA environment, the underlying transport is POSIX shared memory (`/dev/shm`). Resizing cannot be accomplished via a simple `malloc` or `realloc` because the remote client processes must have visibility into the exact file descriptors and memory-mapped physical pages.

This document outlines the precise mechanism to migrate the `resizing` branch into `main` and adapt the POSIX shared memory transport to support dynamic, lock-free, zero-downtime resizing for the Outback dataset layer.

---

## 2. Git Merge Strategy
1. **Target Branch:** Checkout `main` (which contains the stable `_numa.cc` files).
2. **Action:** Run `git merge origin/resizing`.
3. **Conflict Resolution:** 
    - The core `ludo` and `outback` headers will contain the standard data structure resize algorithms. Accept those.
    - Since `client_numa.cc` and `server_numa.cc` did not exist in the `resizing` branch, there will be no direct conflicts there.
4. **Implementation Phase:** Begin implementing the NUMA-specific transport hooks as detailed below.

---

## 3. Server-Side Implementation (`server_numa.cc`)

### 3.1 Region Versioning System
The server cannot safely `mumap` and immediately delete a shared file while clients are still executing operations on it, as this would cause a `SIGSEGV` (Segmentation Fault) on the client side.

Instead, the server will utilize an **append-only rolling window of shared memory fields via file appending or file suffixing.**

### 3.2 Required Changes to `SharedNumaRegistry`
Ensure `xcomm/src/transport/numa_transport.hh` supports version metadata. Currently, it has a `version` field, but it needs an array or strictly mapped offset variables for the *current* and *previous* regions to prevent dangling pointers.

```cpp
struct SharedNumaRegistry {
    uint64_t magic;
    std::atomic<uint64_t> current_version;
    uint64_t ready;
    int numa_node;

    // Array allowing clients to map up to 10 historical resize events
    struct ResizeEpoch {
        size_t region_size;
        size_t num_buckets;
        size_t num_data_entries;
        size_t packed_array_offset;
        size_t ludo_buckets_offset;
        size_t lock_array_offset;
    } epoch[16];

    // Allocator lanes
    uint32_t mem_threads;
    std::atomic<uint64_t> lane_next_free_index[kMaxNumaLanes];
};
```

### 3.3 Server Resizing Execution Flow
When the Ludo load factor is exceeded and the background resize thread triggers:
1. **Allocate New Region:** Create a new POSIX shared memory file appended with the next version integer. 
   - `shm_open("/outback_numa_region_default_v2", ...)`
2. **Configure Truncation:** `ftruncate` this new file to the newly calculated, larger memory size.
3. **Mmap Local Space:** The server mmaps this new file into its local virtual address space.
4. **Data Migration:** Run the core `ludo` resizing logic, copying the old `packed_data` and slots into this new local mapping.
5. **Publish to Epoch Array:** Write the new sizes and offsets into `shared_meta->epoch[2]`.
6. **Atomic Flip:** Perform a memory-fenced atomic increment of `shared_meta->current_version`.

> **Important:** The server **must not** `munmap` or `shm_unlink` the version 1 file immediately. It must wait a grace period (e.g., 5 seconds) to ensure no clients are mid-operation on the old memory layout.

---

## 4. Client-Side Implementation (`client_numa.cc`)

The client needs an efficient way to detect that the server has changed the `current_version` without introducing a lock or a heavy memory barrier into every single microsecond memory access.

### 4.1 Client Epoch Polling
Clients will cache the `current_version` locally in thread-local storage (`thread_local uint64_t local_version`). 

Inside the main `while(running)` benchmark loop loop:
1. Since `numa_search` and `numa_put` operate at sub-microsecond latency, we do not want to check the shared atomic `current_version` on every single operation. 
2. Instead, we use the `coro_factor` batching loop or a modulo counter to check for a version bump once every `N` operations (e.g., every 1,000 operations).

```cpp
if (unlikely(query_i % 1000 == 0)) {
    uint64_t remote_ver = remote_registry->current_version.load(std::memory_order_acquire);
    if (unlikely(remote_ver > local_version)) {
        // A resize happened! 
        handle_client_resize(local_version, remote_ver);
    }
}
```

### 4.2 Handling Client Resize (`handle_client_resize`)
When a version bump is detected by the client:
1. The client looks up the new region parameters in `remote_registry->epoch[remote_ver]`.
2. The client opens the new shared memory file: `shm_open("/outback_numa_region_default_v" + to_string(remote_ver), ...)`
3. The client maps the new memory: `mmap()`.
4. The client updates its global pointers (`remote_ludo_buckets`, `remote_packed_data`) to point to the addresses mapped inside this new file descriptor.
5. The client advances `local_version = remote_ver`.
6. The client unmaps (`munmap`) and closes (`close`) its old file descriptor for the previous version. (It is safe for the client to drop it now, as it will exclusively use the new layout).

---

## 5. Potential Pitfalls & Edge Cases

*   **Atomic Allocator Over-allocation:** During the precise microsecond of the atomic flip, some client threads might query `lane_next_free_index` on the old array. The server should ideally pad the old allocation counters with enough buffer that straggler requests don't corrupt out-of-bounds space during migration.
*   **Volatile Memory Semantics:** The `__sync_fetch_and_add` instructions the clients use to insert must point to the *new* `lane_next_free_index` counters. Because these metrics are stored inside the `SharedNumaRegistry` (which never gets unmapped or resized itself, only its payloads change), the pointers to the lock queues and allocators will persist safely across resizing epochs.
*   **SIGBUS Errors:** If the server truncates or unlinks an old region before a client thread finishes an inflight `numa_search()`, the client OS will throw a `SIGBUS` (Bus Error). The server's delayed garbage collection (`cron_cleanup_old_regions()`) is strictly mandatory.