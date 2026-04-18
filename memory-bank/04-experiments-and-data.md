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
