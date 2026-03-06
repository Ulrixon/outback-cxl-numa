# OUTBACK NUMA Migration Summary

## What Was Changed

This repository has been modified to support **NUMA-based memory disaggregation** as an alternative to RDMA-based disaggregation. The original RDMA functionality remains intact.

### Key Modifications

#### 1. New NUMA Memory Management (`xutils/numa_memory.hh`)
- **NumaRegion**: Allocates memory on specific NUMA nodes (replaces HugeRegion for NUMA)
- **NumaAllocator**: Manages allocation within NUMA regions
- **NumaMemHandle**: Provides shared access to NUMA memory
- Utility functions: `init_numa()`, `bind_to_numa_node()`, `get_current_numa_node()`

#### 2. New NUMA Transport Layer (`xcomm/src/transport/numa_transport.hh`)
- **NumaTransport**: Direct memory access (replaces RDMA UD transport)
- **NumaRPCOp**: Local memory operations (replaces network RPC)
- **ServerNumaState**: Shared server state structure
- **NumaServerManager**: Global registry for server state

#### 3. New Server Implementation (`benchs/outback/server_numa.cc`)
- Allocates data structures on specified NUMA node
- Registers server state for client access
- No network/RDMA setup required
- Direct function calls instead of RPC callbacks

#### 4. New Client Implementation (`benchs/outback/client_numa.cc`)
- Connects to server via shared memory handle
- Direct memory access for GET/PUT/UPDATE/DELETE operations
- No network communication overhead
- Simplified coroutine-free implementation

#### 5. Updated Build System
- **CMakeLists.txt**: Added targets for `client_numa` and `server_numa`
- **build_numa.sh**: New build script for NUMA version
- **setup-env.sh**: Added "numa" mode for setup without RDMA/OFED

#### 6. Documentation
- **README_NUMA.md**: Comprehensive NUMA architecture documentation
- **SETUP_GUIDE.md**: Step-by-step setup instructions for both modes
- **README.md**: Updated with NUMA setup sections
- **quickstart_numa.sh**: Automated setup and convenience scripts

## Quick Comparison

| Aspect | Original (RDMA) | New (NUMA) |
|--------|----------------|------------|
| **Files** | `server.cc`, `client.cc` | `server_numa.cc`, `client_numa.cc` |
| **Transport** | RDMA UD over network | Direct NUMA memory access |
| **Hardware** | RDMA NICs required | Standard CPU with NUMA |
| **Deployment** | Multiple machines | Single machine, multiple NUMA nodes |
| **Setup** | Complex (OFED, NICs) | Simple (libnuma) |
| **Communication** | RPC over RDMA | Direct memory operations |
| **Latency** | ~1-5 μs (network) | ~100-300 ns (memory) |

## Files Changed/Added

### Added Files
```
xutils/numa_memory.hh                      - NUMA memory management
xcomm/src/transport/numa_transport.hh      - NUMA transport layer
outback/trait_numa.hpp                     - NUMA trait definitions
outback/outback_server_numa.hh             - NUMA server operations
outback/outback_client_numa.hh             - NUMA client operations
benchs/outback/server_numa.cc              - NUMA server benchmark
benchs/outback/client_numa.cc              - NUMA client benchmark
build_numa.sh                              - NUMA build script
quickstart_numa.sh                         - Quick setup script
README_NUMA.md                             - NUMA architecture guide
SETUP_GUIDE.md                             - Setup instructions
```

### Modified Files
```
README.md          - Added NUMA setup sections and comparison
setup-env.sh       - Added "numa" mode for setup
CMakeLists.txt     - Added NUMA build targets
```

### Unchanged (Original RDMA Code)
```
benchs/outback/server.cc                   - Original RDMA server
benchs/outback/client.cc                   - Original RDMA client
outback/outback_server.hh                  - Original RDMA server ops
outback/outback_client.hh                  - Original RDMA client ops
outback/trait.hpp                          - Original RDMA traits
xcomm/src/transport/rdma_ud_t.hh           - Original RDMA transport
All other RDMA-related files               - Untouched
```

## How It Works

### Architecture Changes

#### Original RDMA Flow (Unchanged):
```
Client (Machine 1)                    Server (Machine 2)
    │                                      │
    ├─[1] Calculate slot location         │
    ├─[2] Create RPC request              │
    ├─[3] Send via RDMA UD ──────────────>├─[4] Receive RDMA message
    │                                      ├─[5] Process RPC callback
    │                                      ├─[6] Access local memory
    ├─[7] Receive RPC reply <─────────────├─[7] Send RDMA reply
    └─[8] Process response                 │
```

#### New NUMA Flow:
```
Client (NUMA Node 0)                  Server Memory (NUMA Node 1)
    │                                      │
    ├─[1] Calculate slot location         │
    ├─[2] Direct memory read ────────────>│ (ludo_buckets)
    ├─[3] Direct memory read ────────────>│ (packed_data)
    └─[4] Return value                     │
    
    (No network, no RPC, no serialization)
```

### Key Design Decisions

1. **Client-side lookup structure (ludo_lookup_unit)**: Remains local on client
   - Used to calculate bucket/slot locations
   - Matches server's index structure
   - Enables direct addressing

2. **Server-side data structures**: Allocated on remote NUMA node
   - `packed_data`: Actual key-value pairs
   - `ludo_buckets`: Index structure
   - `mutexArray`: Concurrency control

3. **Global server state**: Shared via manager
   - Pointers to server structures
   - Available to all client threads
   - No network registration needed

4. **Synchronization**: Same mutex-based locking
   - Per-bucket mutexes on NUMA node
   - Prevents race conditions
   - Works across NUMA nodes

## Usage Examples

### NUMA Quick Start
```bash
# One-command setup and test
chmod +x quickstart_numa.sh
./quickstart_numa.sh

# Manual setup
./setup-env.sh numa
./build_numa.sh

# Run server (terminal 1)
./run_server_numa.sh 1 1000000 60 ycsbc

# Run client (terminal 2)
./run_client_numa.sh 4 1000000 500000 60 ycsbc
```

### RDMA Original (Still Works)
```bash
# Setup with OFED
./setup-env.sh

# Build
mkdir -p build && cd build
cmake .. && make -j

# Server machine
sudo taskset -c 0 ./build/benchs/outback/server \
  --seconds=120 --nkeys=50000000 --mem_threads=1 --workloads=ycsbc

# Client machine
sudo taskset -c 0-7 ./build/benchs/outback/client \
  --nic_idx=0 --server_addr=192.168.1.2:8888 \
  --seconds=120 --nkeys=50000000 --bench_nkeys=10000000 \
  --threads=8 --workloads=ycsbc
```

## Benefits of NUMA Version

### For Development & Testing
- ✓ No RDMA hardware needed
- ✓ Faster setup (10 min vs 1 hour)
- ✓ Easier debugging (standard tools work)
- ✓ Lower latency for testing

### For Research
- ✓ Simulates memory disaggregation
- ✓ Tests CXL-like scenarios
- ✓ Evaluates index structures
- ✓ Studies NUMA effects

### For Production*
- ✓ Works on standard servers
- ✓ Good for single-machine deployments
- ✓ Useful for memory tiering
- *Note: RDMA version still better for distributed deployments

## Limitations of NUMA Version

1. **Single Machine Only**: Cannot run on separate physical machines
2. **Distance Limited**: Only useful if you have multiple NUMA nodes
3. **Not True Network Disaggregation**: Still shared-memory, not networked
4. **Memory Sharing**: Server and client share the same physical memory system

For true distributed disaggregation, use the original RDMA version.

## Migration Guide: RDMA to NUMA

If you have existing RDMA-based code and want to try NUMA:

```cpp
// Old RDMA code:
auto res = remote_search(key, rpc, sender, lkey, R2_ASYNC_WAIT);

// New NUMA code:
auto res = numa_search(key);
```

Key changes:
- No RPC object needed
- No sender/lkey needed
- No coroutines needed
- Direct function calls

## Performance Expectations

### NUMA Version (Typical)
- **Latency**: 100-300 ns per operation
- **Throughput**: 5-15 M ops/s (depends on threads and workload)
- **Bottlenecks**: NUMA interconnect bandwidth, cache coherency

### RDMA Version (Typical)
- **Latency**: 1-5 μs per operation
- **Throughput**: 2-10 M ops/s (depends on network and threads)
- **Bottlenecks**: Network RTT, RDMA NIC processing

## Future Enhancements

Potential additions to NUMA version:

1. **CXL Support**: Adapt for CXL.mem devices
2. **Persistent Memory**: Use PMEM on NUMA nodes
3. **Hybrid Mode**: Mix NUMA and RDMA
4. **Memory Tiering**: Hot local, cold remote
5. **Better NUMA Affinity**: smarter thread placement

## Testing

Both versions include the same benchmarks:

```bash
# Test NUMA version
./build/benchs/outback/server_numa --nkeys=1000000 --seconds=60 --workloads=ycsbc
./build/benchs/outback/client_numa --nkeys=1000000 --threads=4 --seconds=60 --workloads=ycsbc

# Test RDMA version (original)
./build/benchs/outback/server --nkeys=1000000 --seconds=60 --workloads=ycsbc
./build/benchs/outback/client --server_addr=... --nkeys=1000000 --threads=4 --seconds=60
```

## Support & Documentation

- **README_NUMA.md**: Full NUMA architecture documentation
- **SETUP_GUIDE.md**: Step-by-step setup for both versions
- **README.md**: Overview and comparison
- **Original OUTBACK docs**: For RDMA-specific details

## Summary

This modification adds NUMA support while preserving all original RDMA functionality. Choose the version that fits your hardware and use case:

- **NUMA**: Testing, development, single machine with multiple NUMA nodes
- **RDMA**: Production, distributed deployment, multiple physical machines

Both implementations share the same core data structures (Ludo hash table, packed data) and benchmark framework, making them directly comparable.
