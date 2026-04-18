# 01 — Project Overview

## What this repo is

A reproduction and CXL/NUMA extension of the **Outback** distributed key-value
store (DMPH-indexed RDMA KV) originally evaluated in the Outback paper
(`docs/outback.pdf`).

## Three deliverables

1. **Reproduction.** Re-run the paper's RDMA experiments on CloudLab Apt
   `r320` nodes (3-node cluster: 1 MN + 2 CN) and analyze any discrepancies
   against the paper's Figures 9–17.
2. **CXL/NUMA extension.** Port the same DMPH KV server to a single-node
   2-socket CloudLab `c6420` node, using cross-socket NUMA load/store traffic
   over UPI as a proxy for CXL.mem (no dedicated CXL hardware available).
3. **Comparison.** Quantify per-MN-thread throughput, end-to-end latency,
   memory footprint, and dynamic-resize behaviour across the three setups.

## Headline numerical results (verified against `results/`)

| Metric | Paper Fig.9 | RDMA repro (r320) | CXL/NUMA (c6420) |
|---|---|---|---|
| Peak per-MN-thread throughput | ~5.5–6 MOPS | ~4.5 MOPS | ~118 MOPS @ 32 T |
| End-to-end YCSB-A latency | ~3 µs | ~3 µs | ~0.14 µs |
| Throughput multiplier vs. RDMA repro | 1× | 1× | ~22× (note: not "23×") |
| Latency reduction vs. RDMA | — | — | ~21× |

(Source rows: `results/experiment_results.csv`,
`results/dataset_experiment_results.csv`.)

## Scope guardrails

- We do **not** claim to match the paper's absolute aggregate throughput
  (paper: 9 nodes / 4 MN threads → 17 MOPS; we: 3 nodes / 1 MN thread).
  Comparisons must always be normalized **per MN thread**.
- The CXL extension is an **emulation** via NUMA, not real CXL.mem.
- The reproduction hardware is the same `r320` class as the paper's Figure 10
  cluster, so same-hardware-class comparisons against Fig.10 are valid;
  comparisons against the paper's Fig.9 (Intel `r650` / CX-6 100 Gbps cluster)
  are cross-platform and need explicit caveats.
