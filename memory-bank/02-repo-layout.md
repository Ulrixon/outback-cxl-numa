# 02 — Repo Layout (post-reorg April 2026)

```
outback-cxl-numa/
├── docs/                  # All report/slides assets + reference PDFs + figures
│   ├── Final_Report.tex   # Main IEEE-style report
│   ├── Final_Report.pdf   # 17 pages total (12 pages main content + appendices A–D)
│   ├── Final_Report_backup.tex  / _old.tex   # Historical snapshots
│   ├── Slides.tex         # Beamer (Madrid) deck
│   ├── Slides.pdf         # 27 pages
│   ├── Slides.tex.bak     # Pre-disk-revert backup (April 2026 fix)
│   ├── outback.pdf                                    # Original paper
│   ├── Outback Reproduction and Discrepancy Analysis.pdf
│   ├── Outback exp rdma outcome.pdf  # NOTE: log title misdocuments hardware as D-1548; experiments are actually on r320
│   ├── Outback numa outcome.pdf
│   ├── analysis_figs/     # Plots used in the report (PDF/PNG)
│   └── paper_figs/        # Figures lifted from the paper for comparison
│
├── results/               # All experiment outputs
│   ├── experiment_results.csv         # Main YCSB sweep (workloads × threads × coro)
│   ├── dataset_experiment_results.csv # Dataset-size sweep
│   ├── compute_node_memory.csv        # Memory experiment (load factor × N keys)
│   ├── memory_results/    # (created by run_memory_experiment.sh on next run)
│   └── resize_results/    # Dynamic resize: timeseries + summary + events per thread count
│
├── memory-bank/           # ← you are here
│
├── benchs/                # Per-system clients/servers (outback, race, drtmr, fasst, arc)
├── outback/               # Outback core (DMPH server, client, NUMA variants, resize)
├── race/                  # RACE baseline
├── drtmr/                 # DrTM-R baseline
├── mica/                  # MICA baseline
├── rolex/                 # Learned-index variant
├── xcomm/                 # Networking layer (RDMA verbs wrapper)
├── xutils/                # Utilities (numa_memory.hh, huge_region.hh, …)
├── deps/                  # ludo, progress-cpp, r2, rlib, ycsb
├── datasets/              # Input keys
│
├── build.sh / build_numa.sh   # CMake wrappers (RDMA build / NUMA build)
├── quickstart_numa.sh         # End-to-end launcher for the CXL/NUMA experiment
├── run_experiments.sh         # YCSB sweep → results/experiment_results.csv
├── run_experiments_datasets.sh# Dataset sweep → results/dataset_experiment_results.csv
├── run_memory_experiment.sh   # Memory sweep → results/memory_results/compute_node_memory.csv
├── run_resize_experiment.sh   # Resize timeseries → results/resize_results/
├── setup-env.sh               # Environment vars (HUGE_PAGES, etc.)
├── README.md, README_NUMA.md, SETUP_GUIDE.md, NUMA_*.md, MIGRATION_SUMMARY.md
│       # User-facing docs (kept at root for discoverability)
└── COMMIT_TEMPLATE.txt, GIT_INSTRUCTIONS.md, commit_changes.sh
```

## What moved in the April 2026 reorg

| Old path (root) | New path |
|---|---|
| `Final_Report.*`, `Slides.*`, `*_backup.tex`, `*_old.tex`, `*.tex.bak` | `docs/` |
| `outback.pdf`, `Outback*.pdf` | `docs/` |
| `analysis_figs/`, `paper_figs/` | `docs/` |
| `experiment_results.csv` | `results/` |
| `dataset_experiment_results.csv` | `results/` |
| `compute_node_memory.csv` | `results/` |
| `resize_results/` | `results/resize_results/` |

Scripts that write CSVs were updated to write into `results/` accordingly.
