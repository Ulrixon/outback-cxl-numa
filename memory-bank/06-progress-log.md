# 06 — Progress Log

Append new entries **at the top**. Use ISO date + one-line summary + bullets.

---

## 2026-04-18 — Documentation consolidation
- Moved 5 markdown docs from repo root into `memory-bank/`:
  - `README_NUMA.md` → `08-numa-architecture.md`
  - `NUMA_MIGRATION_DETAILS.md` → `09-numa-migration-details.md`
  - `NUMA_RESIZING_PLAN.md` → `10-numa-resizing-plan.md`
  - `MIGRATION_SUMMARY.md` → `11-numa-migration-summary.md`
  - `SETUP_GUIDE.md` → `12-setup-guide.md`
- Updated `README.md`:
  - Fixed hardware table (CX-3 10 Gbps → CX-3 MX354A 56 Gbps FDR; "10G RoCE" → "56 Gbps FDR InfiniBand"; CPU 8C → 8C/16T; memory 16 GB → 16 GB DDR3-1600).
  - Fixed all CSV / output paths to point under `results/`.
  - Updated `Final_Report.tex` / `Slides.tex` references to `docs/`.
  - Added `docs/`, `results/`, `memory-bank/` rows to the project-structure table.
  - Added a "Further documentation" section pointing into the memory bank.
- Root now has only 4 top-level docs/configs: `README.md`, `GIT_INSTRUCTIONS.md`, `LICENSE`, `COMMIT_TEMPLATE.txt`.

## 2026-04-18 — Repo reorg + memory bank
- Moved all report/slides assets, reference PDFs, and figure folders into
  `docs/`. Moved all CSV outputs and `resize_results/` into `results/`.
- Updated the four `run_*.sh` scripts to write into `results/` so future
  runs land in the right place.
- Verified both PDFs still build cleanly from `docs/` (Final_Report 18 p,
  Slides 27 p).
- Created this memory bank under `memory-bank/`.

## 2026-04-18 — Slides hardware spec consistency fix
- Discovered `Slides.tex` on disk still contained stale "D-1548 / RoCE
  10 Gbps / 64 GB" strings even after earlier "successful" edits — the
  earlier edits had only touched the editor buffer, not the file. Used
  `sed` to commit the changes, backup saved as `Slides.tex.bak`.
- Replacements made (3 frames affected):
  - "Project Goals" subtitle: → `Xeon E5-2450, 16 GB DDR3, MX354A CX-3
    56 Gbps FDR — same hardware class as paper Fig.10`
  - "Three Platforms Compared" table: NIC → `CX-3, 56 Gbps FDR`,
    CPU → `E5-2450, 8C/16T`, Interconnect → `56 Gbps FDR IB`
  - Throughput plot legend: → `RDMA Repro (r320)`
  - Conclusion block: → `RDMA reproduction on r320 (same hardware class
    as paper Fig.10)`

## 2026-04-18 — CPU spec accuracy pass
- `tab:rdma-setup` in `Final_Report.tex`: split `Network` into `NIC (IB)` +
  `NIC (1GbE)` rows, added `Chassis` row, kept `8 cores / 16 threads,
  20 MB L3` per Intel ARK 64611 + CloudLab Apt §13.4.
- Appendix `tab:hw`: CPU → `Xeon E5-2450, 8C/16T, 2.1 GHz, 20 MB L3`.

## 2026-04-18 — Hardware narrative full revert to r320
- Earlier in the day the report/slides had been (incorrectly) rewritten to
  describe the RDMA setup as `D-1548 / 64 GB / RoCE 10 Gbps`, based on the
  PDF log `docs/Outback exp rdma outcome.pdf`. User clarified that the PDF
  log itself misdocuments the hardware — the experiments truly ran on r320.
- Reverted all hardware references in `Final_Report.tex` and (eventually)
  `Slides.tex` to authoritative r320 specs from CloudLab Apt §13.4.
- Restored "exact hardware match with paper Fig.10" framing.
- Restored OOM-at-80M analysis as "16 GB DRAM insufficient" (not
  "huge-page pool exhaustion").
- Restored 60M-keys slowdown analysis as "memory swapping".
- Restored "RDMA (r320)" labels in figure legends and headlines.
- Restored `~2× lower NIC BW vs. paper` in the setup-difference table
  (CX-6 100 Gbps vs CX-3 56 Gbps FDR).

## 2026-04-18 — FDR speed correction (50 → 56 Gbps)
- CloudLab Apt fabric description states "up to 56 Gbps … FDR Infiniband";
  updated all 9 occurrences across both files from 50 Gbps → 56 Gbps.

## (Earlier) — Initial discrepancy audit
- Cross-checked `Final_Report.tex` and `Slides.tex` against:
  - `docs/outback.pdf` (paper)
  - `docs/Outback exp rdma outcome.pdf`
  - `docs/Outback numa outcome.pdf`
  - All CSVs under what is now `results/`
- All numerical claims (per-MN-thread MOPS, end-to-end YCSB latency, memory
  RSS, resize timeseries) confirmed faithful to raw data.
- Only the **hardware narrative** needed correction (see above entries).
