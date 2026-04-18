# Memory Bank — outback-cxl-numa

This folder is the **persistent project memory** for AI coding assistants and
human collaborators working on this repository. Read these files **before**
making changes; update them **after** you make non-trivial changes.

## Files

| File | Purpose |
|---|---|
| [`01-project-overview.md`](01-project-overview.md) | What the project is, goals, deliverables |
| [`02-repo-layout.md`](02-repo-layout.md) | Directory structure (post-reorg) and what lives where |
| [`03-hardware-and-platforms.md`](03-hardware-and-platforms.md) | **Authoritative** hardware specs for all three platforms (paper, RDMA repro, CXL/NUMA) |
| [`04-experiments-and-data.md`](04-experiments-and-data.md) | What each CSV / log directory contains and how to regenerate it |
| [`05-build-and-run.md`](05-build-and-run.md) | Build commands, scripts, and the LaTeX compile recipe |
| [`06-progress-log.md`](06-progress-log.md) | Chronological history of what has been done; append new entries at the top |
| [`07-known-pitfalls.md`](07-known-pitfalls.md) | Mistakes that have already been made — do not repeat them |
| [`08-numa-architecture.md`](08-numa-architecture.md) | NUMA implementation architecture (formerly `README_NUMA.md`) |
| [`09-numa-migration-details.md`](09-numa-migration-details.md) | RDMA→NUMA code-level migration notes |
| [`10-numa-resizing-plan.md`](10-numa-resizing-plan.md) | Design plan for dynamic resizing in NUMA mode |
| [`11-numa-migration-summary.md`](11-numa-migration-summary.md) | Chronological migration log |
| [`12-setup-guide.md`](12-setup-guide.md) | Extended setup walkthrough (formerly `SETUP_GUIDE.md`) |

## Working agreement

1. Treat `03-hardware-and-platforms.md` as the **single source of truth**
   for any hardware claim in `docs/Final_Report.tex` or `docs/Slides.tex`.
2. After any user-visible change, append a dated bullet to `06-progress-log.md`.
3. When you discover a recurring mistake, add it to `07-known-pitfalls.md`.
4. Numerical claims (throughput, latency, memory) must be traceable to a
   specific row in a file under `results/`. If you cannot trace it, do not
   write it.
