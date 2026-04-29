# 03 — Hardware & Platforms (authoritative)

**This file is the single source of truth.** When updating
`docs/Final_Report.tex` or `docs/Slides.tex`, copy specs from here verbatim.

Sources of truth:
1. CloudLab Manual §13.4 (Apt cluster) — primary for r320
2. CloudLab Manual §13.3 (Clemson) — primary for c6420
3. Intel ARK product 64611 — for E5-2450 cache/threads
4. Outback paper Figures 9 and 10 captions — for paper hardware

---

## Platform A: CloudLab Apt `r320` (RDMA reproduction — our setup)

This is the **same hardware class** as the paper's Fig.10 cluster.

| Field | Value |
|---|---|
| Chassis | Dell PowerEdge R320 (Sandy Bridge generation) |
| CPU | 1× Intel Xeon E5-2450 @ 2.1 GHz, 8 cores / 16 threads, 20 MB L3 |
| Sockets / NUMA | Single socket, 1 NUMA node |
| Memory | 16 GB DDR3-1600 (4× 2 GB RDIMMs) per node |
| NIC (IB) | 1× Mellanox MX354A ConnectX-3 dual-port FDR (`mlx4_0`), up to 56 Gbps; 1× QSA adapter for 10 GbE commodity fabric |
| NIC (1 GbE) | Dual-port embedded Broadcom (control network) |
| Disks | 4× 500 GB 7.2K SATA (RAID5) |
| OS | Ubuntu, kernel 5.x, legacy MLNX_OFED 4.9 stack |
| Cluster scale used | 3 nodes (1 MN + 2 CN) |

**Important:** The PDF log file `docs/Outback exp rdma outcome.pdf`
misdocuments the hardware as "Xeon D-1548 / 64 GB / RoCE 10 Gbps". The
**actual** experiments were on r320 as listed above. Do **not** propagate
the D-1548/RoCE description anywhere.

## Platform B: CloudLab Clemson `c6420` (CXL/NUMA extension — our setup)

| Field | Value |
|---|---|
| Chassis | Dell C6420 (Skylake) |
| CPU | 2× Intel Xeon Gold 6142 @ 2.6 GHz, 16 cores / 32 threads each |
| Sockets / NUMA | 2 sockets, 2 NUMA nodes |
| Memory | 384 GB DDR4-2666 |
| NIC | Dual-port Intel X710 10 GbE (unused — single-node experiment) |
| Cross-socket interconnect | Intel UPI, ~150 ns one-way load |
| Cluster scale used | 1 node (NUMA0 = CN, NUMA1 = MN) |

CXL.mem is **emulated** by binding the MN's memory to NUMA1 and the CN
threads to NUMA0; cross-socket loads/stores then traverse UPI as a CXL
proxy.

## Platform C: Outback paper Figure 9 cluster (reference, not our hardware)

| Field | Value |
|---|---|
| CPU | 2x Intel Xeon Platinum 8360Y, 36 cores each (72 cores/node) |
| Memory | 256 GB |
| NIC | Mellanox ConnectX-6, 100 Gbps |
| Topology | 6 nodes total, 2 shards, each shard = 1 MN + 2 CN |
| MN threads | 1 per shard in Fig.9 throughput plot |
| CN threads | Up to 144 per shard (2 CN x 72 cores) |

## Platform D: Outback paper Figure 10 cluster (reference)

Same hardware class as our Platform A (r320 / E5-2450 / MX354A FDR / 56 Gbps).
Differences from our setup are only **scale**, not hardware:

| Aspect | Paper Fig.10 | Our reproduction |
|---|---|---|
| Total nodes | 9 (1 MN + 8 CN) | 3 (1 MN + 2 CN) |
| MN nodes | 1 | 1 |
| MN threads | 4 (on the single MN node) | 1 |
| Max CN threads | 64 (8 CN x 8 threads/CN) | 32 (2 CN x 16 threads/CN) |

## Current `tab:setup-diff` in Final_Report.tex (5-column comparison)

As of 2026-04-28, `tab:setup-diff` has **5 platform columns**: Paper (r650, Fig.9) / Paper (r320, Fig.10) / RDMA (ours) / CXL (c6420) / Expected Impact.
The three verbose per-platform setup tables (tab:paper-setup, tab:rdma-setup, tab:numa-setup) were removed from §III; full specs moved to Appendix A (`\label{app:hw}`).

| Factor | Paper r650 | Paper r320 | RDMA ours | CXL c6420 | Expected Impact |
|---|---|---|---|---|---|
| NIC | CX-6 100 Gbps | CX-3 56 Gbps FDR | CX-3 56 Gbps FDR | N/A (load/store) | Like-for-like vs. Fig.10; 2× lower vs. Fig.9 |
| CPU | Platinum 8360Y 2×36C | E5-2450 8C | E5-2450 8C | Gold 6142 2×16C | RDMA CPU identical to Fig.10 |
| Max threads | 144/shard | 64 (8/node) | 32 (2×16) | 32 (1 socket) | Cannot test Fig.9 high-thread regime |
| MN threads | 1–3 (2 shards) | 4 | 1–3 | none (no MN CPU) | RDMA MN count matches Fig.10 |
| Node count | 6 (2 shards×3) | Up to 9 | 3 | 1 | Fewer CN; lower aggregate throughput |
| Memory | 256 GB | 16 GB DDR3 | 16 GB DDR3 | 384 GB DDR4 | RDMA OOM at 80M (same as Fig.10) |
| Interconnect | PCIe/IB FDR | IB FDR | IB FDR | UPI ~150 ns | CXL ~20× lower latency |

## Forbidden phrasings (do not write these)

- ❌ "Xeon D-1548 / 64 GB / RoCE 10 Gbps" for the RDMA setup
- ❌ "ConnectX-3 10 Gbps" — the CX-3 here runs IB FDR @ 56 Gbps, not 10 Gbps Ethernet
- ❌ "FDR @ 50 Gbps" — FDR is 56 Gbps per CloudLab fabric description
- ❌ "Fig.10 uses 4MN" — Fig.10 uses 1 MN node with 4 MN threads (4 MNT)
- ❌ "huge-page pool exhaustion" as the cause of OOM-at-80M — actual cause is
      r320's 16 GB DRAM limit
