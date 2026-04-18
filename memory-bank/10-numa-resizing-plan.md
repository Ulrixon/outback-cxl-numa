# Final Revised NUMA Resizing Plan for Outback-Compatible Shared-Memory Transport (v2)

## 1. Purpose

This document revises the NUMA resizing plan so it is both:

1. **Architecturally faithful to the Outback paper**
2. **Operationally safe in a POSIX shared-memory / NUMA environment**

The goal is not to claim perfect zero-downtime structural mutation.  
The real goal is:

- safe versioned resizing,
- continued service for reads and eligible updates,
- controlled deferral of structural mutations,
- explicit client coordination,
- and predictable cleanup of obsolete mappings.

This version incorporates the prior review findings and the additional Gemini suggestions, especially around:

- generation-based signaling,
- heartbeat-based dead-client handling,
- memory pressure guarding,
- internal stale-version remap/retry,
- and explicit MakeupGet support.

---

## 2. Design Objectives

### 2.1 Outback Fidelity
Preserve the paper's key resizing semantics:

- early resize signaling,
- two-threshold behavior (`s_slow`, `s_stop`),
- continued `Get` / `Update` where valid on stale structures,
- deferred or retry-based `Insert` / `Delete` during structural migration,
- explicit compute-node acknowledgment before retirement of temporary metadata,
- support for extendible hashing during growth.

### 2.2 NUMA Safety
Adapt those semantics to shared memory safely:

- never resize active payloads in place,
- never `realloc()` shared payload regions,
- never destroy an old region before all readers are safely drained,
- prevent `SIGBUS` / `SIGSEGV` from stale mappings,
- bound memory blow-up during overlapping epochs,
- recover from crashed or zombie clients.

### 2.3 Practical Client API
Hide resizing complexity inside the client transport wrapper whenever possible:

- detect stale versions internally,
- remap automatically,
- retry eligible operations automatically,
- keep user/application code simple,
- but still expose metrics for debugging and benchmarking.

---

## 3. Core Model

The system has two distinct layers:

### 3.1 Stable Control Plane
A permanently mapped shared registry acts as the immutable coordination anchor.

It stores:

- resize state,
- current and previous version,
- epoch metadata,
- generation counter,
- heartbeat / liveness metadata,
- acknowledgment counters,
- threshold values,
- cleanup eligibility,
- directory/global-depth metadata.

### 3.2 Versioned Data Plane
All large payloads live in **versioned shared-memory regions**.

Examples:
- `/outback_numa_region_default_v1`
- `/outback_numa_region_default_v2`
- `/outback_numa_region_default_v3`

A new resize creates a new region instead of modifying the old one in place.

---

## 4. Mapping to Outback Concepts

This plan preserves the main Outback resizing ideas:

- **Compute-heavy metadata** remains logically separate from memory-heavy payload structures.
- **Resize is not a blind remap**; it is a coordinated protocol.
- **Get/Update can continue** during part of resizing if their addressing remains valid.
- **Insert/Delete may be deferred** while the structure is being rebuilt.
- **Acknowledgment is required** before old state is retired.
- **Extendible hashing must remain part of the design** if fidelity to the paper is desired.
- **MakeupGet must be supported** for overflow/cache cases and stale per-bucket seed transitions.

---

## 5. Threshold Policy

Preserve the paper's two-threshold intent.

## 5.1 `s_slow`
Soft threshold:
- indicates resize pressure is building,
- triggers preparation,
- warns clients that remap may happen soon,
- begins orchestration before overflow becomes critical.

## 5.2 `s_stop`
Hard threshold:
- indicates normal structural mutation can no longer safely continue,
- `Insert` / `Delete` must be deferred or forced to retry,
- server prioritizes migration completion.

### Important Note
The exact numeric values should remain configurable.  
Do not hard-code paper-derived percentages until validated against your actual implementation and workload behavior.

---

## 6. Resize State Machine

```cpp
enum class ResizeState : uint32_t {
    NORMAL = 0,
    PRE_RESIZE = 1,
    COPYING = 2,
    READY_TO_SWITCH = 3,
    DRAIN_OLD = 4,
    GC_PENDING = 5
};
```

## 6.1 State Definitions

### `NORMAL`
No resize in progress.

### `PRE_RESIZE`
Soft threshold crossed.  
Server announces upcoming resize but has not yet published a new version.

### `COPYING`
Server allocates and builds the new region.  
Old region continues serving allowed requests.

### `READY_TO_SWITCH`
New region is fully published and visible to clients.  
Clients may map the new region and acknowledge it.

### `DRAIN_OLD`
Server drains remaining old-version activity and replays deferred structural mutations into the new region.

### `GC_PENDING`
Old region is no longer active but remains retained until cleanup conditions are satisfied.

---

## 7. Shared Registry Design

The registry is the permanently mapped control block.

```cpp
enum class OpStatus : uint32_t {
    OK = 0,
    NOT_FOUND = 1,
    PRE_RESIZE = 2,
    RETRY_LATER = 3,
    DEFERRED = 4,
    STALE_VERSION = 5,
    INTERNAL_ERROR = 6
};

struct ClientLiveness {
    std::atomic<uint64_t> last_heartbeat_ns;
    std::atomic<uint64_t> last_seen_generation;
    std::atomic<uint32_t> active;
};

struct ResizeEpoch {
    uint64_t version;
    uint64_t publish_ts_ns;

    size_t region_size;
    size_t num_buckets;
    size_t num_data_entries;

    size_t packed_array_offset;
    size_t ludo_buckets_offset;
    size_t lock_array_offset;

    uint32_t global_depth;
    uint32_t directory_size;
    uint32_t directory_version;
    uint32_t reserved0;

    std::atomic<uint64_t> clients_pending_ack;
    std::atomic<uint64_t> active_readers;
    std::atomic<uint64_t> active_writers;

    std::atomic<uint32_t> published;
    std::atomic<uint32_t> cleanup_allowed;
};

struct SharedNumaRegistry {
    uint64_t magic;
    uint32_t protocol_version;
    std::atomic<uint32_t> resize_state;

    std::atomic<uint64_t> current_version;
    std::atomic<uint64_t> previous_version;

    // Bumped whenever clients should re-check metadata
    std::atomic<uint64_t> generation;

    uint64_t ready;
    int numa_node;

    uint64_t s_slow;
    uint64_t s_stop;

    std::atomic<uint64_t> registered_clients;
    std::atomic<uint64_t> deferred_insert_count;
    std::atomic<uint64_t> deferred_delete_count;

    std::atomic<uint32_t> global_depth;
    std::atomic<uint32_t> directory_version;

    ResizeEpoch epoch[16];
    ClientLiveness clients[kMaxClients];

    uint32_t mem_threads;
    std::atomic<uint64_t> lane_next_free_index[kMaxNumaLanes];
};
```

### Registry Rules
- The registry is never resized in place.
- The registry remains mapped for the life of the process.
- Epoch entries are append-only until cleanup.
- The generation counter is a fast-path hint to reduce unnecessary version/epoch checks.

---

## 8. Generation Counter and Polling Strategy

Pure polling on `current_version` can create unnecessary cache-line contention when many client threads check frequently.

To reduce this, add a **generation counter**:

- the server increments `generation` whenever a resize state transition or publication event requires clients to re-check metadata,
- clients only perform the heavier version/epoch read path when `generation` changes.

### Client Fast Path
Clients should not inspect full epoch metadata on every operation.

Recommended flow:
1. every `N` ops or every bounded time interval,
2. read `generation`,
3. if unchanged, continue fast path,
4. if changed, inspect `current_version`, epoch metadata, and resize state.

### Recommendation
Make these tunable:
- version-check interval in operations,
- time-based polling interval,
- coroutine-batch frequency.

---

## 9. Client Liveness and Zombie Handling

A dead client must not block cleanup forever.

Add per-client heartbeat entries in the registry:
- `last_heartbeat_ns`
- `last_seen_generation`
- `active`

## 9.1 Heartbeat Policy
Each live client periodically updates its heartbeat timestamp.

## 9.2 Zombie Detection
A client is considered stale if:
- heartbeat exceeds timeout, or
- it never acknowledges a version within a configured grace window.

## 9.3 Cleanup Implication
Old epoch cleanup may proceed even if `clients_pending_ack > 0` **only when** the remaining unacked clients are classified as dead/stale by policy.

This prevents memory exhaustion due to zombie processes.

---

## 10. Server-Side Resize Orchestrator

## 10.1 Entry Conditions
A background worker monitors:
- load factor,
- overflow pressure,
- available NUMA memory capacity.

### Before entering `COPYING`
Perform a **memory pressure guard**:
- verify the NUMA node can tolerate temporary overlap of old + new version,
- estimate worst-case footprint,
- refuse or delay resize if overlap would exceed safe capacity,
- surface a metric/alarm if memory headroom is insufficient.

This is important because the shared-memory design may require roughly 2× region footprint during overlap.

---

## 10.2 Resize Flow

### Step 1: `NORMAL -> PRE_RESIZE`
Conditions:
- pressure >= `s_slow`

Actions:
1. set `resize_state = PRE_RESIZE`
2. increment `generation`
3. record target version = `current_version + 1`
4. prepare deferred-mutation policy
5. continue serving normal operations unless escalation is required

### Step 2: `PRE_RESIZE -> COPYING`
Conditions:
- pressure keeps rising or server decides to begin migration

Actions:
1. perform memory pressure guard
2. create new shared-memory region
3. `shm_open()` new versioned file
4. `ftruncate()` to target size
5. `mmap()` locally
6. construct new tables/directory
7. migrate existing payload
8. integrate overflow / deferred migration input
9. populate `epoch[new_version]`
10. initialize `clients_pending_ack = registered_clients`
11. set `published = 0`
12. increment `generation`
13. set `resize_state = COPYING`

### Step 3: Publish New Epoch
Actions:
1. finalize all epoch metadata
2. issue release fence
3. set `epoch[new_version].published = 1`
4. set `previous_version = old_version`
5. set `current_version = new_version`
6. increment `generation`
7. set `resize_state = READY_TO_SWITCH`

### Step 4: Wait for Acknowledgment
Actions:
1. clients remap and decrement `clients_pending_ack`
2. server waits for:
   - full acknowledgment, or
   - grace-window timeout, after which dead clients are filtered by heartbeat policy

### Step 5: `READY_TO_SWITCH -> DRAIN_OLD`
Actions:
1. block new structural writes to old version
2. replay deferred `Insert` / `Delete`
3. drain old-version readers/writers
4. prevent stale writers from mutating old structures

### Step 6: `DRAIN_OLD -> GC_PENDING`
Actions:
1. mark old epoch cleanup-eligible
2. start grace timer
3. wait for local/remote safety conditions
4. reclaim only when policy permits

### Step 7: `GC_PENDING -> NORMAL`
Actions:
1. `munmap()` old server mapping
2. `close()` old fd
3. `shm_unlink()` old named region
4. increment `generation`
5. return to `NORMAL`

---

## 11. Extendible Hashing Requirement

If the goal is fidelity to Outback, resizing must preserve the extendible-hashing layer.

### Required Metadata
- `global_depth`
- directory size
- per-table local depth
- directory version
- directory-to-table mapping

### Recommendation
- keep directory/control metadata in stable control plane or a compact versioned control segment,
- keep large data/table payloads in versioned data regions.

Do not silently replace this with only “one larger flat table” unless you explicitly accept divergence from the paper.

---

## 12. Operation Semantics During Resize

This section is the most important correctness layer.

## 12.1 `Get`
Allowed during:
- `NORMAL`
- `PRE_RESIZE`
- `COPYING`
- `READY_TO_SWITCH`

Behavior:
- serve from currently valid region associated with the client pointer bundle,
- stale-table service is acceptable during copy as long as protocol invariants are preserved.

## 12.2 `Update`
Allowed during:
- `NORMAL`
- `PRE_RESIZE`
- `COPYING`
- possibly `READY_TO_SWITCH` depending on implementation policy

Behavior:
- may continue on the old structure while addressing remains valid,
- must not create ambiguous dual-write outcomes across old/new versions.

## 12.3 `Insert`
Allowed normally in:
- `NORMAL`
- maybe `PRE_RESIZE`

During:
- `COPYING`
- `READY_TO_SWITCH`

Behavior:
- either:
  - enqueue into deferred insert queue, or
  - return retryable status

Do not apply new structural writes to the old region once invariants would be violated.

## 12.4 `Delete`
Same as `Insert`:
- defer or retry during structural migration,
- replay later into the new active version.

---

## 13. Deferred Mutation Queues

Server-side deferral must be explicit.

```cpp
struct DeferredInsert {
    Key key;
    Value value;
    uint64_t enqueue_ts_ns;
};

struct DeferredDelete {
    Key key;
    uint64_t enqueue_ts_ns;
};
```

### Requirements
- bounded queue size,
- backpressure policy,
- replay before final cleanup of old region,
- metrics on queue lag,
- correctness-preserving replay order.

---

## 14. MakeupGet Support

The client/server design must preserve Outback-style **MakeupGet** logic.

This is needed when:
- the requested key is temporarily in overflowed cache,
- the originally addressed slot returns a different key,
- a bucket seed has changed and the key resides in another slot.

### Required Behavior
If initial `Get` returns a KV block whose key does not match:
1. client or server detects mismatch,
2. issue MakeupGet path,
3. check overflow cache first,
4. if not found, scan candidate slots in the hashed bucket,
5. if found via another slot, return updated seed metadata if needed,
6. refresh the client-side copied seed state.

This is essential for correctness during resize pressure and transient cache cases.

---

## 15. Client-Side Wrapper Design

The client transport should hide version-management complexity where safe.

```cpp
struct ClientRegionState {
    uint64_t local_version;
    uint64_t local_generation;

    int region_fd;
    void* region_base;

    void* remote_ludo_buckets;
    void* remote_packed_data;

    uint32_t observed_global_depth;
    uint32_t observed_directory_version;
};
```

## 15.1 Internalized Stale-Version Handling
If the server returns `STALE_VERSION`, the client wrapper should:

1. read registry generation/version,
2. remap if needed,
3. retry the operation internally where safe,
4. only surface failure if remap/retry cannot resolve it.

This keeps resize-specific retry logic out of benchmark/application code.

## 15.2 Automatic Retry Policy
Eligible operations can be retried automatically:
- `Get`
- some `Update`
- some idempotent wrappers

Structural mutations should only be retried automatically if semantics remain safe.

---

## 16. Atomic Pointer Swaps / RCU-Style Handoff

Within a multi-threaded client process, remap must not leave threads with half-updated pointers.

Use a pointer bundle and switch it atomically:
- allocate/map new region,
- compute all internal pointers,
- publish a new bundle,
- retire the old bundle only after no thread can still reference it.

### Recommended Mechanism
Use one of:
- RCU-style grace period,
- generation-based quiescent-state handoff,
- process-local refcounted pointer bundles.

Do not immediately `munmap()` the old mapping if another thread could still be using it.

---

## 17. Acknowledgment Handshake

Once a client has safely mapped the new region and published its local pointer switch:

1. it decrements `epoch[new_version].clients_pending_ack`,
2. it updates its heartbeat and generation fields,
3. it may retire the old bundle later after local grace period.

This is the NUMA analogue of Outback's compute-node acknowledgment step.

---

## 18. Memory Ordering Rules

Publication ordering must be strict.

### Server Publication Order
1. build and fill new region,
2. write all epoch metadata,
3. issue release fence,
4. set `published = 1`,
5. update `current_version`,
6. bump `generation`.

### Client Consumption Order
1. observe generation/version with acquire semantics,
2. verify `published == 1`,
3. read epoch metadata,
4. map region,
5. publish local pointer bundle,
6. acknowledge.

Never publish a new version before all associated metadata is valid.

---

## 19. Garbage Collection and Force-Cleanup Policy

Cleanup is conservative by default.

## 19.1 Normal Cleanup Conditions
Old version is reclaimable only when:
- `clients_pending_ack == 0`, or remaining clients are declared dead,
- `active_readers == 0`,
- `active_writers == 0`,
- configurable grace period has expired.

## 19.2 Force-Cleanup Policy
If a client heartbeat fails or a client does not acknowledge within the grace window:
- classify it as stale/zombie by policy,
- remove it from cleanup blocking,
- proceed with old-region reclamation once remaining safety checks pass.

### Important
Do not treat heartbeat timeout alone as immediate permission to destroy a region unsafely.  
Cleanup must still respect local refcount / grace-period logic.

---

## 20. Observability and Metrics

Metrics are essential because resizing can visibly affect throughput and latency.

## 20.1 Server Metrics
- resize start/end timestamps,
- copy duration,
- remap publication time,
- deferred insert count,
- deferred delete count,
- deferred queue lag,
- acknowledgment wait time,
- drain-old duration,
- GC delay,
- forced zombie cleanup count,
- memory pressure guard failures.

## 20.2 Client Metrics
- generation-check hits,
- version-change detections,
- remap latency,
- stale-version retries,
- automatic retry count,
- local pointer retirement delay,
- heartbeat age.

These metrics are needed to compare resize behavior against performance findings like those discussed in the Outback paper.

---

## 21. Revised Implementation Phases

## Phase 1: Foundation and Alignment
1. merge resize-capable core logic into the main NUMA branch,
2. validate Ludo / DMPH split between compute-heavy and memory-heavy components,
3. add configuration for `s_slow` and `s_stop`,
4. establish versioned shared-memory allocation helpers.

## Phase 2: Registry and Control Plane
1. implement permanent `SharedNumaRegistry`,
2. add epoch table,
3. add generation counter,
4. add heartbeat/liveness table,
5. add cleanup coordination fields,
6. validate protocol versioning.

## Phase 3: Server Resize Orchestrator
1. add memory pressure guard,
2. implement resize state transitions,
3. create versioned regions,
4. preserve extendible hashing,
5. publish epochs with strict ordering,
6. add zombie-client filtering policy.

## Phase 4: Transparent Client Wrapper
1. add periodic generation checks,
2. remap on version change,
3. internalize `STALE_VERSION` handling,
4. atomically swap pointer bundles,
5. decrement acknowledgment counter after safe switch.

## Phase 5: Operation Semantics
1. allow stale `Get` / controlled `Update`,
2. add deferred `Insert` / `Delete` queues,
3. add retry statuses,
4. implement MakeupGet path,
5. ensure no ambiguous old/new structural mutation.

## Phase 6: Cleanup and Observability
1. implement grace-based cleanup,
2. add stale-client timeout handling,
3. add local refcount/grace safety,
4. expose metrics and tracing,
5. stress test repeated resize cycles.

---

## 22. Testing Plan

## 22.1 Unit Tests
- registry state transitions,
- generation bumps,
- epoch publication ordering,
- heartbeat timeout classification,
- acknowledgment decrement,
- deferred queue replay,
- MakeupGet correctness,
- pointer-bundle retirement.

## 22.2 Integration Tests
- 1 client resize,
- multiple-client resize,
- slow client delaying cleanup,
- crashed client/zombie cleanup,
- repeated resize cycles,
- stale `Get` during `COPYING`,
- deferred `Insert` during `COPYING`,
- `STALE_VERSION` remap-and-retry path.

## 22.3 Stress Tests
- 1MN/1CN,
- 1MN/2CN,
- 1MN/4CN,
- high overflow pressure,
- repeated growth,
- memory pressure guard under low headroom,
- concurrent remap storms,
- long-lived stale readers.

## 22.4 Safety Tests
- no `SIGBUS`,
- no `SIGSEGV`,
- old region not unlinked too early,
- no partial epoch publication exposure,
- no dual-write ambiguity across old/new structure.

---

## 23. Final Recommendation

This v2 plan should be the new implementation target.

It improves on the prior plan by adding:

- generation-based signaling,
- heartbeat-based zombie handling,
- memory pressure guarding,
- internalized stale-version remap/retry,
- explicit MakeupGet support,
- stronger cleanup policy wording,
- and clearer alignment with Outback's resize semantics.

---

## 24. Summary

### What this plan provides
- Outback-compatible resizing behavior
- NUMA-safe shared-memory region replacement
- lower risk of stale-mapping crashes
- controlled mutation deferral during migration
- explicit client coordination
- robust cleanup in the presence of dead clients

### What this plan does not claim
- unrestricted lock-free structural mutation during every resize phase
- immediate destruction of old mappings
- zero-cost remapping

### Correct claim
This design provides **a safe, low-disruption, Outback-faithful resizing protocol for a NUMA shared-memory transport**.
