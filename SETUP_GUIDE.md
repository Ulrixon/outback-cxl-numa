# OUTBACK Setup Guide

## Quick Start: Choose Your Setup

### Option 1: NUMA-Based Setup (Recommended for Testing/Development)

**When to use:**
- Single machine with multiple NUMA nodes
- Testing memory disaggregation concepts
- No RDMA hardware available
- Simulating CXL/memory disaggregation

**Advantages:**
- No special hardware required
- Simple setup (no RDMA drivers)
- Lower latency (direct memory access)
- Easier to debug

**Requirements:**
- Linux with multiple NUMA nodes
- `libnuma-dev` package
- Standard build tools

---

### Option 2: RDMA-Based Setup (Original)

**When to use:**
- Distributed deployment (multiple physical machines)
- Real disaggregated memory scenarios
- Have RDMA NICs available

**Requirements:**
- Mellanox RDMA NICs (CX3, CX5, CX6)
- Mellanox OFED drivers
- Network connectivity between machines

---

## NUMA-Based Setup Instructions

### Step 1: Check NUMA Configuration

```bash
# Check if NUMA is available
numactl --hardware

# Expected output should show multiple nodes:
# available: 2 nodes (0-1)
# node 0 cpus: 0 1 2 3 ...
# node 1 cpus: 8 9 10 11 ...
```

If you only see 1 NUMA node, this setup won't work. Consider using a two-socket server or enabling NUMA in BIOS.

### Step 2: Install Dependencies

```bash
# Run setup script for NUMA-only mode
chmod +x setup-env.sh
./setup-env.sh numa
```

This installs:
- Build tools (cmake, g++, clang)
- libnuma-dev (NUMA library)
- boost, gflags, memcached
- abseil-cpp, gtest
- Configures hugepages

**Note:** This mode does NOT install RDMA/OFED drivers.

### Step 3: Build

```bash
# Use the provided build script
chmod +x build_numa.sh
./build_numa.sh

# OR build manually
mkdir -p build && cd build
cmake ..
make client_numa server_numa -j$(nproc)
```

Executables will be at:
- `./build/benchs/outback/client_numa`
- `./build/benchs/outback/server_numa`

### Step 4: Run Benchmark

#### Terminal 1 - Start Server
```bash
# Server allocates KV store on NUMA node 1
sudo ./build/benchs/outback/server_numa \
  --numa_node=1 \
  --nkeys=10000000 \
  --seconds=60 \
  --workloads=ycsbc
```

#### Terminal 2 - Start Client
```bash
# Client runs on NUMA node 0, accesses node 1 memory
sudo taskset -c 0-7 ./build/benchs/outback/client_numa \
  --nkeys=10000000 \
  --bench_nkeys=5000000 \
  --threads=8 \
  --seconds=60 \
  --workloads=ycsbc
```

### Step 5: Monitor Performance

```bash
# In another terminal, monitor NUMA statistics
watch -n 1 'numastat -c | head -20'

# Monitor memory bandwidth
sudo perf stat -e node-loads,node-stores,node-load-misses sleep 10
```

---

## RDMA-Based Setup Instructions (Original)

### Step 1: Check RDMA Hardware

```bash
# List RDMA devices
ibv_devices

# Check device info
ibv_devinfo
```

### Step 2: Install Dependencies and OFED

```bash
# Run full setup (includes OFED installation)
chmod +x setup-env.sh
./setup-env.sh
```

This takes longer as it downloads and installs Mellanox OFED drivers.

### Step 3: Configure Network

```bash
# Server machine
sudo ifconfig <interface> 192.168.1.2 netmask 255.255.0.0

# Client machine
sudo ifconfig <interface> 192.168.1.1 netmask 255.255.0.0

# Test RDMA bandwidth
# Server:
ib_write_bw -d <device> --report_gbits

# Client:
ib_write_bw 192.168.1.2 -d <device> --report_gbits
```

### Step 4: Build

```bash
# Build original RDMA version
mkdir -p build && cd build
cmake ..
make client server -j$(nproc)
```

### Step 5: Run Benchmark

#### Server Machine
```bash
sudo taskset -c 0 ./build/benchs/outback/server \
  --seconds=120 \
  --nkeys=50000000 \
  --mem_threads=1 \
  --workloads=ycsbc
```

#### Client Machine
```bash
sudo taskset -c 0-7 ./build/benchs/outback/client \
  --nic_idx=0 \
  --server_addr=192.168.1.2:8888 \
  --seconds=120 \
  --nkeys=50000000 \
  --bench_nkeys=10000000 \
  --coros=2 \
  --mem_threads=1 \
  --threads=8 \
  --workloads=ycsbc
```

---

## Workload Types

Both setups support the same workloads:

- **ycsba**: 50% reads, 50% updates
- **ycsbb**: 95% reads, 5% updates
- **ycsbc**: 100% reads
- **ycsbd**: 95% reads, 5% inserts (latest distribution)
- **ycsbf**: 50% reads, 50% read-modify-write

---

## Troubleshooting

### NUMA Setup Issues

**Problem:** `NUMA is not available on this system`

**Solutions:**
1. Check BIOS settings - enable NUMA
2. Use a two-socket server (single socket usually has only 1 NUMA node)
3. Check with: `dmesg | grep -i numa`

**Problem:** `Failed to allocate NUMA memory`

**Solutions:**
1. Run with sudo
2. Check available memory: `numactl --hardware`
3. Reduce `--nkeys` parameter

**Problem:** `Cannot connect to NUMA server`

**Solutions:**
1. Ensure server is running
2. Check that server has registered state (see server logs)
3. Ensure both processes run on same machine

### RDMA Setup Issues

**Problem:** `RDMA device not found`

**Solutions:**
1. Check `lsmod | grep mlx` - OFED drivers loaded?
2. Run `sudo /etc/init.d/openibd restart`
3. Check `ibv_devices` output

**Problem:** `Connection timeout`

**Solutions:**
1. Check network connectivity: `ping 192.168.1.2`
2. Check RDMA connectivity: `rping -s` on server, `rping -c -a 192.168.1.2` on client
3. Check firewall settings

---

## Performance Tuning

### NUMA Optimizations

```bash
# Disable automatic NUMA balancing
echo 0 | sudo tee /proc/sys/kernel/numa_balancing

# Increase hugepages
echo 8192 | sudo tee /proc/sys/vm/nr_hugepages

# Pin threads to specific NUMA node
numactl --cpunodebind=0 --membind=0 ./client_numa ...
```

### RDMA Optimizations

```bash
# Tune interrupt coalescing
sudo ethtool -C <interface> adaptive-rx off adaptive-tx off

# Increase send/receive queue sizes
# (Edit CMakeLists.txt or code)
```

---

## Comparison Matrix

| Aspect | NUMA Setup | RDMA Setup |
|--------|------------|------------|
| **Setup Time** | 10-15 minutes | 30-60 minutes |
| **Hardware Cost** | $0 (standard server) | $500-2000 (RDMA NICs) |
| **Deployment** | Single machine | Multiple machines |
| **Latency** | ~100-300ns | ~1-5μs |
| **Bandwidth** | 40-100 GB/s | 55-100 Gb/s |
| **Debugging** | Easy (standard tools) | Complex (RDMA tools) |
| **Use Case** | Testing, CXL simulation | Production disaggregation |

---

## Next Steps

After successful setup:

1. **Read the architecture docs**: See [README_NUMA.md](README_NUMA.md) for NUMA details
2. **Experiment with parameters**: Try different workloads and thread counts
3. **Monitor performance**: Use `numastat`, `perf`, and `top`
4. **Benchmark comparison**: Compare NUMA vs RDMA performance if you have both

---

## Support

For issues:
1. Check logs for error messages
2. Verify hardware configuration
3. Review troubleshooting section above
4. Check original OUTBACK documentation for RDMA specifics
