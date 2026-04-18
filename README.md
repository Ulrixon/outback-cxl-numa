## OUTBACK — RDMA Reproduction & CXL/NUMA Extension

Fast and Communication-efficient Index for Key-Value Store on Disaggregated Memory.
Built on top of Rolex/XStore/R2.

This repository contains:
1. **RDMA reproduction** of the original Outback paper on CloudLab r320 nodes
2. **CXL/NUMA extension** that replaces RDMA with cross-socket NUMA loads/stores (emulating CXL.mem)
3. **Experiment scripts** to reproduce all figures from the report

> **Report & Slides:** See [`docs/Final_Report.tex`](docs/Final_Report.tex) and [`docs/Slides.tex`](docs/Slides.tex).
>
> **For contributors / future maintainers:** Read the [`memory-bank/`](memory-bank/) folder before making changes. It contains the authoritative project state, hardware specs, experiment data schemas, build recipes, and a log of past mistakes.

---

## Table of Contents

- [Quick Start](#quick-start)
- [CloudLab Hardware](#cloudlab-hardware)
- [Replicate RDMA Experiments (Part 1)](#replicate-rdma-experiments-part-1)
- [Replicate CXL/NUMA Experiments (Part 2)](#replicate-cxlnuma-experiments-part-2)
- [Replicate Memory Usage Experiment (Fig. 16)](#replicate-memory-usage-experiment-fig-16)
- [Replicate Resize Experiment (Fig. 17)](#replicate-resize-experiment-fig-17)
- [Architecture Overview](#architecture-overview)

---

## Quick Start

```bash
# Clone
git clone https://github.com/Ulrixon/outback-cxl-numa.git
cd outback-cxl-numa

# --- For NUMA/CXL experiments (single multi-socket machine) ---
./setup-env.sh numa
./build_numa.sh

# --- For RDMA experiments (requires 3 CloudLab r320 nodes) ---
./setup-env.sh
./build.sh
```

---

## CloudLab Hardware

| | **Paper (r650)** | **RDMA Repro (r320)** | **CXL/NUMA (c6420)** |
|---|---|---|---|
| NIC | CX-6 100 Gbps | CX-3 MX354A 56 Gbps FDR | N/A (load/store) |
| CPU | Gold 6338N 32C | E5-2450 8C/16T | 2× Gold 6142 16C |
| Memory | 256 GB | 16 GB DDR3-1600 | 384 GB DDR4 |
| Nodes | Up to 9 | 3 (1 MN + 2 CN) | 1 (2 NUMA sockets) |
| Interconnect | PCIe/RDMA | 56 Gbps FDR InfiniBand | UPI ~150 ns |

To instantiate on CloudLab:
- **RDMA (r320):** Create a 3-node experiment with `r320` profile. Each node has a Mellanox MX354A ConnectX-3 dual-port FDR adapter on the Apt 56 Gbps fabric.
- **CXL/NUMA (c6420):** Create a single `c6420` node. Verify 2 NUMA nodes with `numactl --hardware`.

> Authoritative specs and the rationale for these numbers live in [`memory-bank/03-hardware-and-platforms.md`](memory-bank/03-hardware-and-platforms.md).

---

## Replicate RDMA Experiments (Part 1)

### 1. Setup (on all 3 r320 nodes)

```bash
git clone https://github.com/Ulrixon/outback-cxl-numa.git
cd outback-cxl-numa
./setup-env.sh        # installs OFED, dependencies
./build.sh            # builds RDMA client/server
```

### 2. Verify RNIC connectivity

```bash
# On server (MN):
sudo ifconfig ibp8s0 192.168.1.2 netmask 255.255.0.0
ib_write_bw --report_gbits

# On client (CN):
sudo ifconfig ibp8s0 192.168.1.0 netmask 255.255.0.0
ib_write_bw 192.168.1.2 -d mlx4_0 -i 1 -D 10 --report_gbits
```

Expected: ~55 Gbps on r320.

### 3. Run YCSB throughput sweep (Report Fig. 10–12)

```bash
# On MN (server):
sudo taskset -c 0 ./build/benchs/outback/server \
  --seconds=500 --nkeys=50000000 --mem_threads=1 --workloads=ycsbc

# On CN (client) — sweep threads 1,2,4,8,16:
for threads in 1 2 4 8 16; do
  sudo taskset -c 0-$((threads-1)) ./build/benchs/outback/client \
    --nic_idx=0 \
    --server_addr=192.168.1.2:8888 \
    --seconds=120 \
    --nkeys=50000000 \
    --bench_nkeys=10000000 \
    --coros=2 \
    --mem_threads=1 \
    --threads=$threads \
    --workloads=ycsbc
done
```

Repeat with `--workloads=ycsba`, `ycsbb`, `ycsbd` for all workloads.

> **Note:** On r320 use `--nic_idx=0`. The `--mem_threads` value must match on both client and server.

### 4. Automated sweep

```bash
./run_experiments.sh          # YCSB throughput sweep → results/experiment_results.csv
./run_experiments_datasets.sh # FB/OSM dataset sweep  → results/dataset_experiment_results.csv
```

---

## Replicate CXL/NUMA Experiments (Part 2)

### 1. Setup (on c6420)

```bash
git clone https://github.com/Ulrixon/outback-cxl-numa.git
cd outback-cxl-numa
./setup-env.sh numa     # no OFED needed
./build_numa.sh          # builds client_numa / server_numa
```

Verify NUMA topology:
```bash
numactl --hardware
# Should show: available: 2 nodes (0-1)
```

### 2. Run YCSB throughput sweep (Report Fig. 13–15)

**Terminal 1 — Server** (allocates KV data on NUMA node 1):
```bash
sudo ./build/benchs/outback/server_numa \
  --numa_node=1 \
  --nkeys=50000000 \
  --seconds=500 \
  --workloads=ycsbc
```

**Terminal 2 — Client** (runs on NUMA node 0, accesses remote memory on node 1):
```bash
# Sweep: threads = 1, 2, 4, 8, 16, 32
for threads in 1 2 4 8 16 32; do
  sudo taskset -c 0-$((threads-1)) ./build/benchs/outback/client_numa \
    --nkeys=50000000 \
    --bench_nkeys=10000000 \
    --threads=$threads \
    --seconds=120 \
    --workloads=ycsbc
done
```

Repeat with `--workloads=ycsba`, `ycsbb`, `ycsbd`.

### 3. Automated sweep

```bash
./run_experiments.sh   # also supports NUMA mode — outputs results/experiment_results.csv
```

### Parameters

| Flag | Description | Default |
|------|-------------|---------|
| `--numa_node` | NUMA node for server memory allocation | 1 |
| `--nkeys` | Total keys to bulk-load | 50000000 |
| `--bench_nkeys` | Keys used during benchmark | 10000000 |
| `--threads` | Client worker threads | 8 |
| `--seconds` | Benchmark duration (s) | 120 |
| `--workloads` | YCSB workload (ycsba/b/c/d/f) | ycsbc |

> **Tip:** Use `taskset` to pin client threads to NUMA node 0 CPUs for cross-socket measurement.

---

## Replicate Memory Usage Experiment (Fig. 16)

Measures compute-node DMPH index size across load factors and key counts. No server needed.

```bash
./run_memory_experiment.sh
# Output: results/memory_results/compute_node_memory.csv
```

Sweeps:
- **Load factors:** 0.80, 0.85, 0.90, 0.95
- **Key counts:** 20M, 40M, 60M, 80M, 100M

Expected: step-wise jumps due to power-of-2 bucket allocation (e.g., 64 MB at 60M keys vs paper's predicted 34 MB).

---

## Replicate Resize Experiment (Fig. 17)

Measures throughput time series during hash table resizing under YCSB-D (5% insert / 95% read).

```bash
./run_resize_experiment.sh
# Output: results/resize_results/
#   resize_timeseries_t{8,12,16}.csv   — per-second throughput
#   resize_events_t{8,12,16}.txt       — server-side resize state transitions
#   resize_summary.csv                 — min/mean/max per run
```

Configuration:
- 20M keys bulk-loaded, YCSB-D workload
- Resize thresholds: `s_slow=0.58`, `s_stop=0.65` (of 2×NKEYS = 40M slots)
- Thread counts: 8, 12, 16

Expected: RDMA shows ~45% throughput drop for 2–3s; CXL/NUMA shows ~95% stall for 1–2s with instant recovery.

---

## Architecture Overview

### Outback DMPH Architecture

```
Compute Node (CN)                    Memory Node (MN)
┌──────────────────────┐            ┌──────────────────────────┐
│ DataPlaneLudo (DMPH)  │            │ ControlPlaneLudo (full)  │
│  ├─ Othello XOR arrays│            │ LudoBuckets              │
│  └─ Seed array (1B/bkt)│           │  └─ [fp|len|addr] × 4   │
│                        │            │ PackedData               │
│ Lookup:               │   1 RPC    │  └─ [key|value] array    │
│  h(k) → 2 buckets    │──────────→ │ LRU overflow cache       │
│  Othello → pick one   │            │ RPC handlers             │
│  seed+hash → slot 0-3 │←────────── │  (GET/PUT/UPDATE/DEL)    │
│                        │   value    │                          │
│ ~16 MB for 20M keys   │            │ All KV data lives here   │
└──────────────────────┘            └──────────────────────────┘
```

### NUMA Mode (CXL emulation)

```
NUMA Node 0 (CN — compute)          NUMA Node 1 (MN — memory)
┌──────────────────────┐            ┌──────────────────────────┐
│ Client threads        │            │ KV data (mmap'd)         │
│ DMPH index (local)    │◄──────────┤ PackedData + LudoBuckets │
│ Direct pointer deref  │ UPI ~150ns │                          │
└──────────────────────┘            └──────────────────────────┘
   MOV instruction (no RPC)           Allocated via set_mempolicy()
```

### RDMA Mode (original)

```
Machine 1 (CN)                       Machine 2 (MN)
┌──────────────────────┐            ┌──────────────────────────┐
│ Client + DMPH index   │◄──────────┤ Server + KV data         │
│ RDMA QP               │   RDMA    │ Registered memory        │
└──────────┬───────────┘            └──────────┬───────────────┘
       [RDMA NIC]─────── Network ────────[RDMA NIC]
```

---

## Project Structure

| Path | Description |
|------|-------------|
| `outback/` | Core Outback implementation (client, server, DMPH, resize) |
| `outback/*_numa.hh` | CXL/NUMA-specific variants |
| `benchs/outback/` | Benchmark drivers (`client.cc`, `server.cc`, `*_numa.cc`) |
| `deps/ludo/` | Ludo Cuckoo Hash + Othello (the DMPH engine) |
| `run_experiments.sh` | YCSB throughput sweep |
| `run_experiments_datasets.sh` | FB/OSM dataset experiments |
| `run_resize_experiment.sh` | Resize time-series experiment (Fig. 17) |
| `run_memory_experiment.sh` | DMPH memory usage experiment (Fig. 16) |
| `docs/` | LaTeX report + slides + reference PDFs + figures |
| `docs/Final_Report.tex` | Full LaTeX report (IEEE format) |
| `docs/Slides.tex` | Beamer presentation slides |
| `results/` | All experiment output CSVs and resize logs |
| `memory-bank/` | Project memory for AI assistants and contributors |

## Further documentation

Detailed design / setup / migration documents have been consolidated under [`memory-bank/`](memory-bank/):

- [`memory-bank/03-hardware-and-platforms.md`](memory-bank/03-hardware-and-platforms.md) — authoritative hardware specs
- [`memory-bank/05-build-and-run.md`](memory-bank/05-build-and-run.md) — build recipes including LaTeX
- [`memory-bank/08-numa-architecture.md`](memory-bank/08-numa-architecture.md) — NUMA implementation architecture (formerly `README_NUMA.md`)
- [`memory-bank/09-numa-migration-details.md`](memory-bank/09-numa-migration-details.md) — RDMA→NUMA code-level migration notes
- [`memory-bank/10-numa-resizing-plan.md`](memory-bank/10-numa-resizing-plan.md) — design plan for dynamic resizing in NUMA mode
- [`memory-bank/11-numa-migration-summary.md`](memory-bank/11-numa-migration-summary.md) — chronological migration log
- [`memory-bank/12-setup-guide.md`](memory-bank/12-setup-guide.md) — extended setup walkthrough (formerly `SETUP_GUIDE.md`)

---

## License

See [LICENSE](LICENSE).
