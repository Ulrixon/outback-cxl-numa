#!/usr/bin/env bash
# ============================================================================
# run_resize_experiment.sh
#
# Reproduces the Outback paper Fig. 17 resize throughput experiment:
#
#   "We bulk-load 20M keys to warm up, set one compute node with 8, 12, and
#    16 threads connecting to the memory node running only one thread.
#    Workload: YCSB D (5% insert / 95% read).  It takes ~3 s to recalculate
#    the bucket locator; throughput drops ~52% during resizing."
#
# Resize trigger strategy
# -----------------------
# The server's packed array is sized at 2×NKEYS = 40M slots.  After bulk-
# loading NKEYS=20M keys the table is 50% full.  To force resize early enough
# to observe within a 120-second window we set:
#   s_slow_pct = 0.58  →  s_slow  = 23.2M  (need ~3.2M new inserts)
#   s_stop_pct = 0.65  →  s_stop  = 26.0M  (need ~6.0M new inserts)
# With 8 client threads at typical YCSB-D throughput (~2 M ops/sec combined,
# 5% inserts = ~100 K inserts/sec) the resize triggers around t ≈ 30 s.
#
# Output files (written to RESULTS_DIR)
# -------
#   resize_timeseries_t<N>.csv   Per-second throughput for each thread count
#   resize_events_t<N>.txt       Server-side resize state transitions with timestamps
#   resize_summary.csv           Min/Mean/Max throughput + latency per run
# ============================================================================
set -euo pipefail

# ────────────────────────────────────────────────────────────────────────────
# Experiment parameters  (edit these to match your machine)
# ────────────────────────────────────────────────────────────────────────────

BUILD_DIR="./build"                # CMake build output directory
RESULTS_DIR="./resize_results"     # Where all output files go
NKEYS=20000000                     # 20 M bulk-load keys (matches paper)
BENCH_NKEYS=20000000               # ops pool size; large enough for 120 s
NON_NKEYS=2000000                  # 2 M new-key pool for YCSB-D inserts
WORKLOAD="ycsbd"                   # YCSB D: 95% Get, 5% Insert
MEM_THREADS=1                      # Memory-node server thread count
COROS=4                            # Client coroutines per thread
EXP_SECONDS=120                    # Benchmark duration per run (seconds)
SERVER_STARTUP_WAIT=120            # Max seconds to wait for server ready signal
SERVER_SHUTDOWN_WAIT=5             # Seconds after SIGINT for shm cleanup

# Resize thresholds (fraction of packed_entries = 2×NKEYS = 40 M)
# s_slow=0.58 → 23.2 M  /  s_stop=0.65 → 26.0 M
# Resize triggers ~30 s into the benchmark run.
S_SLOW_PCT=0.58
S_STOP_PCT=0.65

# Thread counts to test (matching paper: 8, 12, 16)
THREAD_COUNTS=(8 12 16)

# NUMA topology
#   SERVER_CPU  : physical core for the server process
#   SERVER_MEM  : NUMA memory node for server allocations (CXL / remote node)
#   SERVER_NUMA_NODE : passed to --numa_node flag (same as SERVER_MEM)
#   CLIENT_MEM  : NUMA memory node for client allocations (local / CPU node)
SERVER_CPU=0
SERVER_MEM=1
SERVER_NUMA_NODE=1
CLIENT_MEM=0

# ────────────────────────────────────────────────────────────────────────────
# Sanity checks
# ────────────────────────────────────────────────────────────────────────────

SERVER_BIN="${BUILD_DIR}/benchs/outback/server_numa"
CLIENT_BIN="${BUILD_DIR}/benchs/outback/client_numa"

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "ERROR: server binary not found at $SERVER_BIN"
    echo "  Run: cmake -B build -DCMAKE_BUILD_TYPE=Release . && cmake --build build -j\$(nproc)"
    exit 1
fi
if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "ERROR: client binary not found at $CLIENT_BIN"
    exit 1
fi

if ! command -v numactl &>/dev/null; then
    echo "ERROR: numactl not found (install numautils / numactl)"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

# ────────────────────────────────────────────────────────────────────────────
# Helper: build a comma-separated core list for numactl --physcpubind
# Starts at core 2 (skips core 0 where the server runs) and steps by 2
# to use one hardware thread per physical core.
# ────────────────────────────────────────────────────────────────────────────
get_client_cores() {
    local n=$1
    local cores=""
    local c=2
    # If 32+ threads are requested we must start at core 0 to fit
    if (( n >= 32 )); then c=0; fi
    for (( i=1; i<=n; i++ )); do
        [[ $i -eq 1 ]] && cores="$c" || cores="${cores},$c"
        c=$(( c + 2 ))
    done
    echo "$cores"
}

# ────────────────────────────────────────────────────────────────────────────
# Helper: extract per-second throughput time-series from a client log
# Expected log line format (from run_benchmark()):
#   [Info] [micro] >>> sec N throughput: X, latency: Y us
# ────────────────────────────────────────────────────────────────────────────
extract_timeseries() {
    local logfile=$1
    local outcsv=$2
    echo "second,throughput_ops,latency_us" > "$outcsv"
    grep '\[micro\] >>> sec' "$logfile" | \
        sed 's/.*>>> sec \([0-9]*\) throughput: \([0-9]*\), latency: \([0-9.]*\).*/\1,\2,\3/' \
        >> "$outcsv"
}

# ────────────────────────────────────────────────────────────────────────────
# Helper: extract resize state-transition events from server log
# Server prints:  [resize] state -> STATE  ts_ms=MILLIS
# ────────────────────────────────────────────────────────────────────────────
extract_resize_events() {
    local logfile=$1
    local outtxt=$2
    echo "# Resize state transitions" > "$outtxt"
    grep '\[resize\] state' "$logfile" >> "$outtxt" || echo "  (none recorded)" >> "$outtxt"
}

# ────────────────────────────────────────────────────────────────────────────
# Summary CSV header
# ────────────────────────────────────────────────────────────────────────────
SUMMARY_CSV="${RESULTS_DIR}/resize_summary.csv"
echo "Threads,NKeys,SSlowPct,SStopPct,MeanTput_MOPs,MaxTput_MOPs,MinTput_MOPs,MeanLat_us,P95Lat_us,P99Lat_us,P999Lat_us,ResizeTrigger_s,NormalRecovery_s" \
    > "$SUMMARY_CSV"

# ────────────────────────────────────────────────────────────────────────────
# Per-run function
# ────────────────────────────────────────────────────────────────────────────
run_resize_trial() {
    local THREADS=$1
    local CLIENT_CORES
    CLIENT_CORES=$(get_client_cores "$THREADS")

    local TAG="t${THREADS}"
    local SERVER_LOG="${RESULTS_DIR}/server_${TAG}.log"
    local CLIENT_LOG="${RESULTS_DIR}/client_${TAG}.log"
    local TIMESERIES_CSV="${RESULTS_DIR}/resize_timeseries_${TAG}.csv"
    local EVENTS_TXT="${RESULTS_DIR}/resize_events_${TAG}.txt"

    echo ""
    echo "========================================================================"
    echo "  RESIZE EXPERIMENT  |  client threads=${THREADS}  |  mem_threads=${MEM_THREADS}"
    echo "  s_slow_pct=${S_SLOW_PCT}  s_stop_pct=${S_STOP_PCT}"
    echo "  Expected: resize triggers ~30 s in, ~3 s duration, ~52% tput drop"
    echo "========================================================================"

    # ── Clean up stale shared-memory artefacts ─────────────────────────────
    rm -f /dev/shm/outback_numa_*

    # ── Start server ────────────────────────────────────────────────────────
    echo "[$(date +%T)] Starting server on core ${SERVER_CPU}, NUMA mem node ${SERVER_MEM} ..."
    sudo numactl \
        --physcpubind=${SERVER_CPU} \
        --membind=${SERVER_MEM} \
        "${SERVER_BIN}" \
        --numa_node=${SERVER_NUMA_NODE} \
        --nkeys=${NKEYS} \
        --seconds=$(( EXP_SECONDS + 60 )) \
        --mem_threads=${MEM_THREADS} \
        --workloads=${WORKLOAD} \
        --s_slow_pct=${S_SLOW_PCT} \
        --s_stop_pct=${S_STOP_PCT} \
        > "${SERVER_LOG}" 2>&1 &
    SERVER_PID=$!

    echo "[$(date +%T)] Waiting for server to be ready (no timeout)..."
    local waited=0
    local server_ready=0
    while true; do
        sleep 2
        waited=$(( waited + 2 ))

        # Bail only if the server process truly vanished
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            echo "ERROR: server exited prematurely after ${waited} s.  Check ${SERVER_LOG}"
            cat "${SERVER_LOG}" | tail -20
            return 1
        fi

        # The server logs "resize orchestrator started" once it is fully ready
        if grep -q 'resize orchestrator started' "${SERVER_LOG}" 2>/dev/null; then
            server_ready=1
            echo "[$(date +%T)] Server ready after ${waited} s."
            break
        fi

        # Progress report every 30 s
        if (( waited % 30 == 0 )); then
            echo "[$(date +%T)] Still waiting for server... (${waited} s elapsed)"
        fi
    done

    # ── Start client ────────────────────────────────────────────────────────
    echo "[$(date +%T)] Starting client with ${THREADS} threads on cores ${CLIENT_CORES} ..."
    sudo numactl \
        --physcpubind="${CLIENT_CORES}" \
        --membind=${CLIENT_MEM} \
        "${CLIENT_BIN}" \
        --nkeys=${NKEYS} \
        --non_nkeys=${NON_NKEYS} \
        --bench_nkeys=${BENCH_NKEYS} \
        --threads=${THREADS} \
        --seconds=${EXP_SECONDS} \
        --mem_threads=${MEM_THREADS} \
        --coros=${COROS} \
        --workloads=${WORKLOAD} \
        > "${CLIENT_LOG}" 2>&1
    CLIENT_EXIT=$?

    # ── Graceful server shutdown ────────────────────────────────────────────
    echo "[$(date +%T)] Client done (exit=${CLIENT_EXIT}).  Shutting down server ..."
    sudo kill -INT "${SERVER_PID}" 2>/dev/null || true
    sleep "${SERVER_SHUTDOWN_WAIT}"
    # Force-kill if still alive
    sudo kill -KILL "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true

    # ── Parse results ───────────────────────────────────────────────────────
    extract_timeseries "${CLIENT_LOG}" "${TIMESERIES_CSV}"
    extract_resize_events "${SERVER_LOG}" "${EVENTS_TXT}"

    # Summary metrics
    MEAN_TPUT=$(grep 'Mean Throughput(MOPs)'  "${CLIENT_LOG}" | awk '{print $NF}')
    MAX_TPUT=$(grep  'Max Throughput(MOPs)'   "${CLIENT_LOG}" | awk '{print $NF}')
    MIN_TPUT=$(grep  'Min Throughput(MOPs)'   "${CLIENT_LOG}" | awk '{print $NF}')
    MEAN_LAT=$(grep  'Mean Latency(us)'       "${CLIENT_LOG}" | awk '{print $NF}')
    P95_LAT=$(grep   'Tail Latency P95(us)'   "${CLIENT_LOG}" | awk '{print $NF}')
    P99_LAT=$(grep   'Tail Latency P99(us)'   "${CLIENT_LOG}" | awk '{print $NF}')
    P999_LAT=$(grep  'Tail Latency P999(us)'  "${CLIENT_LOG}" | awk '{print $NF}')

    # Detect the second at which resize was first triggered and when NORMAL
    # was restored — these are derived from the *client* per-second log by
    # correlating with the server's [resize] timestamps.
    RESIZE_TRIGGER_S="n/a"
    NORMAL_RECOVERY_S="n/a"
    if [[ -s "${EVENTS_TXT}" ]]; then
        # Server boot timestamp (first line containing ts_ms in SERVER_LOG)
        BOOT_MS=$(grep 'ts_ms=' "${SERVER_LOG}" | head -1 | grep -oP 'ts_ms=\K[0-9]+' || echo "0")
        # PRE_RESIZE timestamp
        PRE_MS=$(grep 'state -> PRE_RESIZE' "${SERVER_LOG}" | head -1 | grep -oP 'ts_ms=\K[0-9]+' || echo "0")
        # Final NORMAL timestamp (after GC)
        NORM_MS=$(grep 'state -> NORMAL' "${SERVER_LOG}" | tail -1 | grep -oP 'ts_ms=\K[0-9]+' || echo "0")
        if [[ "$BOOT_MS" -gt 0 && "$PRE_MS" -gt 0 ]]; then
            # Subtract warmup period to get seconds-into-benchmark
            WARMUP_END_MS=$(grep 'resize orchestrator started' "${SERVER_LOG}" | \
                grep -oP 'ts_ms=\K[0-9]+' 2>/dev/null | head -1 || echo "$BOOT_MS")
            RESIZE_TRIGGER_S=$(( (PRE_MS  - WARMUP_END_MS) / 1000 ))
            [[ "$NORM_MS" -gt 0 ]] && \
                NORMAL_RECOVERY_S=$(( (NORM_MS - WARMUP_END_MS) / 1000 ))
        fi
    fi

    for v in MEAN_TPUT MAX_TPUT MIN_TPUT MEAN_LAT P95_LAT P99_LAT P999_LAT; do
        [[ -z "${!v}" ]] && eval "$v=ERROR"
    done

    echo "\"${THREADS}\",$NKEYS,${S_SLOW_PCT},${S_STOP_PCT},${MEAN_TPUT},${MAX_TPUT},${MIN_TPUT},${MEAN_LAT},${P95_LAT},${P99_LAT},${P999_LAT},${RESIZE_TRIGGER_S},${NORMAL_RECOVERY_S}" \
        >> "$SUMMARY_CSV"

    echo ""
    echo "  Results:"
    printf "    Mean: %-10s  Max: %-10s  Min: %-10s  MOPs\n" \
        "${MEAN_TPUT}" "${MAX_TPUT}" "${MIN_TPUT}"
    printf "    Mean latency: %-10s  P99: %-10s  us\n" "${MEAN_LAT}" "${P99_LAT}"
    printf "    Resize trigger at t=%-5s s   Normal restored at t=%-5s s\n" \
        "${RESIZE_TRIGGER_S}" "${NORMAL_RECOVERY_S}"
    echo "  Per-second time series → ${TIMESERIES_CSV}"
    echo "  Resize events          → ${EVENTS_TXT}"
}

# ────────────────────────────────────────────────────────────────────────────
# Main: iterate over thread counts
# ────────────────────────────────────────────────────────────────────────────

echo "========================================================================"
echo "  Outback NUMA resize throughput experiment (paper Fig. 17 replication)"
echo "  NKEYS=${NKEYS}  workload=${WORKLOAD}  mem_threads=${MEM_THREADS}"
echo "  thread counts: ${THREAD_COUNTS[*]}"
echo "  s_slow_pct=${S_SLOW_PCT}  s_stop_pct=${S_STOP_PCT}"
echo "  results → ${RESULTS_DIR}/"
echo "========================================================================"

for T in "${THREAD_COUNTS[@]}"; do
    run_resize_trial "$T"
done

# ────────────────────────────────────────────────────────────────────────────
# Post-experiment: quick per-second analysis to confirm ~52% tput drop
# ────────────────────────────────────────────────────────────────────────────
echo ""
echo "========================================================================"
echo "  Per-thread tput-drop analysis"
echo "  (baseline = first 20 s, nadir = lowest 3-s window during resize)"
echo "========================================================================"

for T in "${THREAD_COUNTS[@]}"; do
    CSV="${RESULTS_DIR}/resize_timeseries_t${T}.csv"
    [[ ! -f "$CSV" ]] && continue

    # Compute average of seconds 5-25 (post-warmup pre-resize baseline)
    BASELINE=$(awk -F',' 'NR>1 && $1>=5 && $1<=25 {sum+=$2; n++} END {if(n>0) printf "%.0f", sum/n; else print "0"}' "$CSV")
    # Compute minimum throughput in window seconds 26-90
    NADIR=$(awk    -F',' 'NR>1 && $1>=26 && $1<=90 {if(min==""||$2<min) min=$2} END {printf "%.0f", min+0}' "$CSV")
    if [[ "$BASELINE" -gt 0 && "$NADIR" -gt 0 ]]; then
        DROP_PCT=$(awk "BEGIN {printf \"%.1f\", (1 - $NADIR/$BASELINE)*100}")
    else
        DROP_PCT="n/a"
    fi
    printf "  threads=%-3s  baseline=%-10s ops/s  nadir=%-10s ops/s  drop=%s%%\n" \
        "$T" "$BASELINE" "$NADIR" "$DROP_PCT"
done

echo ""
echo "Resize experiment complete.  Summary → ${SUMMARY_CSV}"
