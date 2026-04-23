# 05 — Build & Run

## C++ build (RDMA path)

```bash
./build.sh        # CMake + make → build/benchs/{outback,race,drtmr,fasst,arc,rolex}/
```

## C++ build (NUMA / CXL-emulation path)

```bash
./build_numa.sh   # Builds outback/client_numa and outback/server_numa
```

## Run experiments

| Goal | Command | Output |
|---|---|---|
| Main YCSB sweep | `./run_experiments.sh` | `results/experiment_results.csv` |
| Dataset-size sweep | `./run_experiments_datasets.sh` | `results/dataset_experiment_results.csv` |
| Memory footprint | `./run_memory_experiment.sh` | `results/memory_results/compute_node_memory.csv` |
| Dynamic resize | `./run_resize_experiment.sh` | `results/resize_results/` |
| One-shot CXL/NUMA demo | `./quickstart_numa.sh` | console |

Most scripts source `setup-env.sh` for huge-page / NUMA-bind environment
variables. Read it before changing thread counts.

## LaTeX build (report + slides)

The `.tex` files are now in `docs/` and are self-contained (no
`\graphicspath`, no external `\includegraphics` — all figures are TikZ /
pgfplots).

```bash
cd docs
pdflatex -interaction=nonstopmode -halt-on-error Final_Report.tex
pdflatex -interaction=nonstopmode -halt-on-error Final_Report.tex   # second pass for cross-refs / TOC
pdflatex -interaction=nonstopmode -halt-on-error Slides.tex
pdflatex -interaction=nonstopmode -halt-on-error Slides.tex
```

Expected outputs:
- `docs/Final_Report.pdf` — 18 pages
- `docs/Slides.pdf` — 24 pages

`latexmk` is **not installed** in this WSL environment; use the two-pass
`pdflatex` recipe above.

## TikZ gotcha already fixed (do not re-introduce)

In `Slides.tex` the DMPH-Lookup frame defined a `step/.style=` key that
collided with TikZ's built-in `step` key in pgfkeys. It is now renamed to
`stepbox/.style=`. If you add new TikZ styles, avoid the names: `step`,
`grid`, `loop`, `pin`.
