## OUTBACK
built on top of Rolex/XStore/R2..

Fast and Communication-efficient Index for Key-Value Store on Disaggregated Memory.

## Deployment Options

This repository supports two deployment modes:

1. **RDMA-based** (original): Requires RDMA NICs for network-based disaggregation
2. **NUMA-based** (new): Uses NUMA nodes for local memory disaggregation (no RDMA hardware required)

---

## NUMA-Based Setup (Recommended for Single Machine with Multiple NUMA Nodes)

### Prerequisites
- Linux system with multiple NUMA nodes (check with `numactl --hardware`)
- libnuma-dev library
- Standard build tools (cmake, g++, etc.)

### Setup Environment
```bash
# Install dependencies (NUMA-only, no RDMA required)
./setup-env.sh numa

# Check NUMA configuration
numactl --hardware
```

### Build
```bash
# Build NUMA version
./build_numa.sh

# OR manually:
mkdir -p build && cd build
cmake ..
make client_numa server_numa -j$(nproc)
```

### Run NUMA Benchmark

#### Start Server (allocates KV store on NUMA node 1)
```bash
sudo ./build/benchs/outback/server_numa \
  --numa_node=1 \
  --nkeys=50000000 \
  --seconds=120 \
  --workloads=ycsbc
```

#### Start Client (runs on NUMA node 0, accesses server memory on node 1)
```bash
sudo taskset -c 0-7 ./build/benchs/outback/client_numa \
  --nkeys=50000000 \
  --bench_nkeys=10000000 \
  --threads=8 \
  --seconds=120 \
  --workloads=ycsbc
```

**Parameters:**
- `--numa_node`: NUMA node for server memory allocation (default: 1)
- `--nkeys`: Total number of keys to load
- `--bench_nkeys`: Number of keys to use in benchmark
- `--threads`: Number of client threads
- `--seconds`: Benchmark duration
- `--workloads`: Workload type (ycsba, ycsbb, ycsbc, ycsbd, ycsbf)

**Note:** Use `taskset` to bind client threads to CPUs on local NUMA node for optimal performance.

See [README_NUMA.md](README_NUMA.md) for detailed NUMA architecture documentation.

---

## RDMA-Based Setup (Original - Requires RDMA Hardware)

### Build
```
./setup-env.sh
cd outback
./build.sh
```

### Test RNIC
* CloudLab r650 w./ Mlnx CX6 100 Gb NIC (~92.57Gbits):
    ```
    server:
    sudo ifconfig ens2f0 192.168.1.2 netmask 255.255.0.0
    ib_write_bw -d mlx5_2 -i 1 -D 10 --report_gbits
    ```
    ```
    client:
    sudo ifconfig ens2f0 192.168.1.0 netmask 255.255.0.0
    ib_write_bw 192.168.1.2 -d mlx5_2 -i 1 -D 10 --report_gbits
    ```
* Cloudlab r320 w./ Mlnx MX354A FDR CX3 adapter (~55.52Gbits):
    ```
    server:
    sudo ifconfig ibp8s0 192.168.1.2 netmask 255.255.0.0
    ib_write_bw --report_gbits
    ```
    ```
    client:
    sudo ifconfig ibp8s0 192.168.1.0 netmask 255.255.0.0
    ib_write_bw 192.168.1.2 -d mlx4_0 -i 1 -D 10 --report_gbits
    ```

### Run RDMA throughput benchmark
```
server:
sudo taskset -c 0 ./build/benchs/outback/server --seconds=120 --nkeys=50000000 --mem_threads=1 --workloads=ycsbc
```
``` 
client:
sudo taskset -c 0-$((threads-1)) ./build/benchs/outback/client --nic_idx=2 --server_addr=192.168.1.2:8888 --seconds=120 --nkeys=50000000 --bench_nkeys=10000000 --coros=2 --mem_threads=1 --threads=$threads --workloads=ycsbc
```
Note that if you use r320, the ```--nic_idx``` should be set as 0, also parameter ```--mem_threads``` should be the same in both client and server, ```numactl --physcpubind=0-71``` may also works, ```--threads``` not larger than 71.

---

## Comparison: NUMA vs RDMA

| Feature | NUMA Version | RDMA Version |
|---------|--------------|--------------|
| **Hardware Requirements** | Standard CPU with NUMA | RDMA NIC (Mellanox CX3/CX6) |
| **Network** | Not required (same machine) | Required (separate machines) |
| **Setup Complexity** | Simple (memory allocation) | Complex (OFED, QP, MR) |
| **Latency** | Memory access (~100-300ns) | Network RTT (~1-5us) |
| **Bandwidth** | NUMA interconnect (40-100 GB/s) | RDMA link (55-100 Gb/s) |
| **Cost** | Standard hardware | Expensive RDMA NICs |
| **Use Case** | Single machine, CXL testing | Distributed systems |

## Architecture Overview

### NUMA Architecture (New)
```
NUMA Node 0 (CPU + DRAM)          NUMA Node 1 (DRAM)
┌─────────────────────────┐      ┌─────────────────────────┐
│ Client Process          │      │ Server Data Structures  │
│ - Compute (CN)          │◄─────┤ - packed_data          │
│ - Index cache           │ NUMA │ - ludo_buckets         │
│ - Thread stacks         │Access│ - KV pairs             │
└─────────────────────────┘      └─────────────────────────┘
```

### RDMA Architecture (Original)
```
Machine 1 (Client/CN)             Machine 2 (Server/MN)
┌─────────────────────────┐      ┌─────────────────────────┐
│ Client Process          │      │ Server Process          │
│ - Index cache           │◄─────┤ - packed_data          │
│ - RDMA QP               │ RDMA │ - ludo_buckets         │
│ - Local memory          │  UD  │ - Registered memory    │
└─────────────────────────┘      └─────────────────────────┘
         │                                  │
    [RDMA NIC]────────Network────────[RDMA NIC]
```
