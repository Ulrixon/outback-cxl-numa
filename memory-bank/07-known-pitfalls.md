# 07 — Known Pitfalls

If you encounter any of these, **stop and re-read the relevant section** of
this memory bank before "fixing" it.

## Pitfall 1: Trusting the PDF experiment logs for hardware specs

`docs/Outback exp rdma outcome.pdf` contains a hardware preamble that says
"Xeon D-1548 / 64 GB / RoCE 10 Gbps". **This is wrong.** The experiments
actually ran on CloudLab Apt `r320` (E5-2450 / 16 GB / MX354A FDR 56 Gbps).
The numerical results inside the PDF are correct; only the hardware
description is wrong.

→ See [`03-hardware-and-platforms.md`](03-hardware-and-platforms.md).

## Pitfall 2: `replace_string_in_file` "succeeding" but not writing to disk

When VS Code has a `.tex` file open with unsaved buffer state that already
matches the new string, `replace_string_in_file` may report success
without actually changing the file on disk that `pdflatex` reads.

**Symptom:** PDF still shows old content after a "successful" recompile.
**Mitigation:** After any `.tex` edit, verify with `pdftotext file.pdf - |
grep <expected new string>`. If absent, fall back to `sed -i` to write
directly to disk.

## Pitfall 3: TikZ key name collisions

`Slides.tex` once had `step/.style=` which collided with TikZ's built-in
`step` pgfkey, producing a confusing pgfkeys error in the DMPH-Lookup
frame. Renamed to `stepbox/.style=`. Avoid built-in TikZ key names
(`step`, `grid`, `loop`, `pin`, `at`, `from`, `to`).

## Pitfall 4: Cross-platform throughput comparison

Do **not** directly compare absolute MOPS between:
- Paper Fig.9 (Intel `r650` / CX-6 100 Gbps / 6 nodes total,
  2 shards x (1 MN + 2 CN), 1 MNT/shard)
- Paper Fig.10 (r320 / CX-3 56 Gbps FDR / 9 nodes total,
  1 MN + 8 CN, 4 MN threads on one MN node)
- Our reproduction (r320 / CX-3 56 Gbps FDR / 3 nodes,
  1 MN + 2 CN, 1 MN thread)

Always normalize to **per-MN-thread throughput** before comparing. Same
goes for the CXL/NUMA results (single-node, no NIC → ~118 MOPS @ 32 T).

## Pitfall 5: FDR is 56 Gbps, not 50 Gbps

CloudLab's Apt fabric description: "up to 56 Gbps … FDR Infiniband".
Mellanox FDR line rate is 56 Gbps (14 Gbps per lane × 4 lanes). Do not
write "50 Gbps FDR".

## Pitfall 6: OOM root cause on r320

When the dataset reaches ~80 M keys at LF=0.95, server processes are killed
by OOM. The root cause is the **r320 node's 16 GB DRAM ceiling**, not
huge-page pool exhaustion. The 60 M-key slowdown that precedes it is
swap-driven, not GC or rebuild.

## Pitfall 7: Don't claim "23×" — it's "22×"

The CXL/NUMA peak-throughput multiplier vs. the RDMA reproduction is
~22× (118 MOPS / ~4.5 MOPS = 26.2× peak per-MN-thread, but the report
quotes the workload-averaged 22× figure). The latency reduction is ~21×.
Earlier drafts of the slides said "23×" — this should be "22×" to match
the report.

## Pitfall 8: Scripts write to `results/` now

After the April 2026 reorg, the four `run_*.sh` scripts write into
`results/`. If you re-introduce a script that writes a CSV at the repo
root, it will be missed by anyone looking under `results/`.

## Pitfall 9: "4MN" wording is ambiguous and usually wrong

For paper Fig.10, use "4 MNT" or "4 MN threads on one MN node".
Do not write "4MN" unless you explicitly mean 4 separate MN nodes.

## Pitfall 10: Do not silently merge 1CN and 2CN curves

When plotting RDMA reproduction against paper Fig.9/Fig.10, keep 1CN and 2CN
as separate curves unless explicitly asked to merge them.

Recommended visual convention:
- filled markers = 1CN
- hollow markers = 2CN
- same color = same workload
