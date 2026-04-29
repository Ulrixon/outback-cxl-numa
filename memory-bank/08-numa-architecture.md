# OUTBACK with NUMA Support

## Overview

This is a modified version of the OUTBACK key-value store that uses NUMA (Non-Uniform Memory Access) instead of RDMA (Remote Direct Memory Access) for memory disaggregation.

### Key Changes

#### Original Architecture (RDMA-based)
- **Client (CN)**: Sends remote operations via RDMA UD (Unreliable Datagram)
- **Server (MN)**: Receives RDMA requests and processes them via RPC callbacks
- **Communication**: Network-based RDMA verbs (IBV_*), RNic, QP, RPC layer
- **Memory**: Registered with RDMA NICs using HugeRegion

#### New Architecture (NUMA-based)
- **Client (CN)**: Runs on local DRAM, directly accesses server memory on remote NUMA node
- **Server (MN)**: Data structures (`packed_data`, `ludo_buckets`) allocated on remote NUMA node
- **Communication**: Direct memory access via NUMA, no network overhead
- **Memory**: NUMA-aware allocation using `libnuma`

## Files Changed/Added

### New Files Created

1. **xutils/numa_memory.hh** - NUMA memory management layer
   - `NumaRegion`: NUMA memory allocator replacing HugeRegion
   - `NumaAllocator`: Memory allocator for NUMA regions
   - `NumaMemHandle`: Handle for accessing NUMA memory
   - Utility functions: `init_numa()`, `bind_to_numa_node()`, etc.

2. **xcomm/src/transport/numa_transport.hh** - NUMA transport layer
   - `NumaTransport`: Replaces RDMA UD transport
   - `NumaRPCOp`: Direct memory operations instead of RPC
   - `ServerNumaState`: Shared server state structure
   - `NumaServerManager`: Global manager for server state

3. **outback/trait_numa.hpp** - NUMA-based trait definitions
   - Removes RDMA dependencies
   - Includes NUMA memory headers

4. **outback/outback_server_numa.hh** - NUMA-based server operations
   - Direct function callbacks replacing RPC callbacks
   - `outback_get_direct()`, `outback_put_direct()`, etc.

5. **outback/outback_client_numa.hh** - NUMA-based client operations
   - Direct memory access functions
   - `numa_search()`, `numa_put()`, `numa_update()`, `numa_remove()`

6. **benchs/outback/server_numa.cc** - NUMA server benchmark
   - Allocates data structures on specified NUMA node
   - Registers server state for client access

7. **benchs/outback/client_numa.cc** - NUMA client benchmark
   - Connects to server NUMA memory
   - Direct memory access for all operations

### Modified Files

1. **CMakeLists.txt** - Added NUMA build targets
   - `client_numa` and `server_numa` executables
   - Already includes `numa` library in `LOG_LIBRARIES`

## Build Instructions

### Prerequisites

- **libnuma-dev**: NUMA library
  ```bash
  sudo apt-get install libnuma-dev
  ```

- A system with multiple NUMA nodes (check with `numactl --hardware`)

### Build

```bash
./build.sh
# OR
mkdir -p build && cd build
cmake ..
make client_numa server_numa
```

This will create:
- `./build/benchs/outback/client_numa`
- `./build/benchs/outback/server_numa`

## Usage

### Check NUMA Configuration

```bash
# Check available NUMA nodes
numactl --hardware

# Check current NUMA policy
numactl --show
```

### Running the Server

The server allocates its key-value store data structures on a specified NUMA node:

```bash
# Run server on NUMA node 1
sudo ./build/benchs/outback/server_numa \
  --numa_node=1 \
  --nkeys=50000000 \
  --seconds=120 \
  --workloads=ycsbc
```

Parameters:
- `--numa_node`: NUMA node ID for server memory (default: 1)
- `--nkeys`: Number of keys to load
- `--seconds`: Benchmark duration
- `--workloads`: Workload type (ycsba, ycsbc, etc.)

### Running the Client

The client runs on local DRAM (typically NUMA node 0) and directly accesses the server's NUMA memory:

```bash
# Run client with multiple threads
sudo taskset -c 0-7 ./build/benchs/outback/client_numa \
  --nkeys=50000000 \
  --bench_nkeys=10000000 \
  --threads=8 \
  --seconds=120 \
  --workloads=ycsbc
```

Parameters:
- `--threads`: Number of client threads
- `--nkeys`: Total number of keys
- `--bench_nkeys`: Number of keys to use in benchmark
- `--seconds`: Benchmark duration
- `--workloads`: Workload type

**Note**: Use `taskset` to bind client threads to CPUs on the local NUMA node for best performance.

## Architecture Details

### Memory Layout

```
NUMA Node 0 (Local CPU/DRAM)          NUMA Node 1 (Remote DRAM)
┌──────────────────────────┐         ┌──────────────────────────┐
│  Client Process          │         │  Server Data Structures  │
│  - ludo_lookup_unit      │         │  - packed_data          │
│  - Thread stacks         │         │  - ludo_buckets         │
│  - Local data            │         │  - mutexArray           │
└──────────────────────────┘         └──────────────────────────┘
           │                                      ▲
           │  Direct Memory Access                │
           └──────────────────────────────────────┘
                    (via NUMA)
```

### Data Structures

1. **packed_data** (on NUMA node): Actual key-value pairs
2. **ludo_buckets** (on NUMA node): Index structure using Ludo hash table
3. **ludo_lookup_unit** (local): Used by client to determine slot locations
4. **mutexArray** (on NUMA node): Per-bucket locks for concurrency control

### Operation Flow

#### GET Operation (NUMA version)
1. Client calls `numa_search(key)`
2. Client calculates slot location using local `ludo_lookup_unit`
3. Client directly reads from `ludo_buckets` on NUMA node
4. Client directly reads value from `packed_data` on NUMA node
5. Returns value (no network, no RPC)

#### PUT Operation (NUMA version)
1. Client calls `numa_put(key, value)`
2. Client calculates bucket location
3. Client acquires mutex on NUMA node
4. Client checks slot availability in `ludo_buckets`
5. Client writes to `packed_data` and updates `ludo_buckets`
6. Client releases mutex

## Performance Considerations

### Advantages of NUMA vs RDMA
1. **Lower Latency**: Direct memory access, no network stack
2. **Simpler**: No RDMA NIC setup, no QP management
3. **Cheaper**: No special RDMA hardware required
4. **Debugging**: Easier to debug with standard tools

### Expected Performance
- **Latency**: Sub-microsecond for local NUMA node access
- **Throughput**: Limited by memory bandwidth and NUMA interconnect
- **Scalability**: Depends on NUMA topology

### Optimization Tips
1. **CPU Binding**: Bind client threads to CPUs on local NUMA node
   ```bash
   numactl --cpunodebind=0 --membind=0 ./client_numa ...
   ```

2. **Server Memory Binding**: Ensure server memory is on different NUMA node
   ```bash
   ./server_numa --numa_node=1 ...
   ```

3. **Hugepages**: Enable hugepages for better TLB performance
   ```bash
   echo 8192 | sudo tee /proc/sys/vm/nr_hugepages
   ```

4. **Cache Optimization**: Minimize false sharing between client and server

## Comparison with Original RDMA Version

| Feature | RDMA Version | NUMA Version |
|---------|--------------|--------------|
| Hardware | RDMA NIC required | Standard CPU/memory |
| Network | Required | Not required (same machine) |
| Latency | Network RTT | Memory access latency |
| Setup | Complex (QP, MR, etc.) | Simple (memory allocation) |
| Distance | Remote machines | Same machine (NUMA nodes) |
| Cost | Expensive RDMA NICs | Standard hardware |

## Troubleshooting

### NUMA not available
```
Error: NUMA is not available on this system
```
**Solution**: Install libnuma and ensure your system has multiple NUMA nodes

### Permission denied
```
Error: Failed to allocate NUMA memory
```
**Solution**: Run with sudo or adjust permissions

### Cannot connect to server
```
Error: Failed to connect to NUMA server
```
**Solution**: Ensure server is running and has registered its state

## Future Enhancements

1. **CXL Support**: Extend to use CXL (Compute Express Link) for memory disaggregation
2. **NUMA-aware Scheduling**: Intelligent thread placement
3. **Memory Tiering**: Hot data on local NUMA, cold data on remote NUMA
4. **Persistent Memory**: Support for PMEM on NUMA nodes

## References

- Original OUTBACK paper: Fast and Communication-efficient Index for Key-Value Store on Disaggregated Memory
- libnuma documentation: https://www.kernel.org/doc/html/latest/vm/numa.html
- NUMA best practices: https://documentation.suse.com/sles/15-SP1/html/SLES-all/cha-tuning-numactl.html
