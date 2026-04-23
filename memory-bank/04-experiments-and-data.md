# 04 — Experiments & Data

All experiment outputs live under `results/`. Each file's schema and source
script are documented below.

## `results/experiment_results.csv`

**Source:** `run_experiments.sh`
**What it is:** Main YCSB throughput sweep across workloads, thread counts,
and coroutine counts. This is the primary input for the report's Section 4
RDMA-vs-CXL throughput plots.

Sweep ranges:
- Workloads: A, B, C, D, F (YCSB)
- Threads: 1, 2, 4, 8, 16, 32
- Coroutines: 1, 2, 3
- Run length: 120 s per cell

## RDMA plotting convention (Slides + Final Report)

As of 2026-04-21, RDMA reproduction curves in `docs/Slides.tex` and
`docs/Final_Report.tex` are plotted as two explicit topology series per
workload (not a merged line):

- 1CN series: 1 MN + 1 CN, filled markers, threads {1,2,4,8,16}
- 2CN series: 1 MN + 2 CN, hollow markers, threads {4,8,16,32}
- Same color = same workload across 1CN/2CN/Fig.9/Fig.10
- Paper references: Fig.9 uses dashed lines; Fig.10 uses dotted lines

Canonical coordinates from `results/experiment_results.csv` (coro=2):

| Workload | 1CN points (T,MOPS) | 2CN points (T,MOPS) |
|---|---|---|
| C | (1,0.724) (2,1.411) (4,2.770) (8,4.503) (16,4.325) | (4,2.802) (8,4.269) (16,4.833) (32,5.040) |
| B | (1,0.731) (2,1.451) (4,2.836) (8,3.722) (16,4.091) | (4,2.777) (8,4.043) (16,4.461) (32,4.439) |
| A | (1,0.740) (2,1.471) (4,2.746) (8,3.632) (16,3.783) | (4,2.709) (8,3.649) (16,3.873) (32,4.049) |
| D | (1,0.744) (2,1.460) (4,2.699) (8,3.832) (16,3.900) | (4,2.720) (8,3.982) (16,4.229) (32,4.281) |
| F | (1,0.626) (2,1.194) (4,1.939) (8,2.213) (16,2.370) | (4,1.926) (8,2.157) (16,2.501) (32,2.354) |

## `results/dataset_experiment_results.csv`

**Source:** `run_experiments_datasets.sh`
**What it is:** Throughput vs. dataset size (number of keys), used to show
the OOM transition at ~80 M keys on r320 (16 GB DRAM ceiling) and the swap
slowdown that begins at ~60 M keys.

## `results/compute_node_memory.csv`

**Source:** `run_memory_experiment.sh`
**What it is:** DMPH memory footprint as a function of `(load_factor, N_keys)`.
Load factors swept: 0.80, 0.85, 0.90, 0.95.
Key counts: 20 M, 40 M, 60 M, 80 M, 100 M.

Note: future runs will land in `results/memory_results/` (subdirectory created
by the script). The historical CSV at `results/compute_node_memory.csv` is the
data used in the current report.

## `results/resize_results/`

**Source:** `run_resize_experiment.sh`
**What it is:** Dynamic-resize timeseries across thread counts {8, 12, 16}.

Per thread count `T`:
- `resize_timeseries_t<T>.csv` — per-second throughput during resize
- `resize_events_t<T>.txt` — resize event log (start, mid-points, completion)

Plus one summary file:
- `resize_summary.csv` — total resize duration and steady-state throughput
  before/after for each thread count

## How to verify a numerical claim

1. Open the relevant CSV.
2. Filter by the exact (workload, threads, coroutine, dataset) tuple cited in
   the report.
3. Quote the actual row in the report's caption / table.
4. Never round in a way that changes the leading significant digit.

## Reference PDFs (read-only, in `docs/`)

- `outback.pdf` — original Outback paper
- `Outback Reproduction and Discrepancy Analysis.pdf` — earlier write-up of
  the discrepancy analysis (predates the current report)
- `Outback exp rdma outcome.pdf` — RDMA experiment log; **the hardware
  description in this PDF is wrong** (says D-1548; actually r320). The
  numerical results inside it are correct.
- `Outback numa outcome.pdf` — NUMA/CXL experiment log
