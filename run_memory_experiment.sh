#!/usr/bin/env bash
# ============================================================================
# run_memory_experiment.sh
#
# Measures Outback compute-node DMPH memory usage (paper Fig. 16 replication).
#
# Sweeps:
#   load_factor : 0.80, 0.85, 0.90, 0.95  (0.05 interval)
#   nkeys       : 20M, 40M, 60M, 80M, 100M (20M interval)
#
# No server required.  client_numa --memory_only builds the DMPH table locally
# and exits, printing the measured sizes.
#
# Output: memory_results/compute_node_memory.csv
# ============================================================================
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-./build}"
CLIENT_BIN="${BUILD_DIR}/benchs/outback/client_numa"
RESULTS_DIR="./memory_results"
OUTPUT_CSV="${RESULTS_DIR}/compute_node_memory.csv"

LOAD_FACTORS=(0.80 0.85 0.90 0.95)
NKEYS_LIST=(20000000 40000000 60000000 80000000 100000000)

# ── Sanity checks ─────────────────────────────────────────────────────────────
if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "ERROR: client binary not found at $CLIENT_BIN"
    echo "  Run: ./build_numa.sh"
    exit 1
fi

mkdir -p "$RESULTS_DIR"

# ── CSV header ────────────────────────────────────────────────────────────────
echo "nkeys,load_factor,num_buckets,seeds_MB,othello_MB,total_MB" > "$OUTPUT_CSV"

echo "========================================================================"
echo "  Outback compute-node DMPH memory experiment (paper Fig. 16)"
echo "  load_factors: ${LOAD_FACTORS[*]}"
echo "  nkeys:        ${NKEYS_LIST[*]}"
echo "  results → ${OUTPUT_CSV}"
echo "========================================================================"
printf "\n%-12s  %-12s  %-14s  %-10s  %-12s  %-10s\n" \
    "NKeys" "LoadFactor" "NumBuckets" "Seeds(MB)" "Othello(MB)" "Total(MB)"
echo "------------------------------------------------------------------------"

for lf in "${LOAD_FACTORS[@]}"; do
    for nk in "${NKEYS_LIST[@]}"; do
        # Run client in memory_only mode; capture stderr (LOG output)
        log_output=$(
            "${CLIENT_BIN}" \
                --nkeys="${nk}" \
                --non_nkeys=1 \
                --bench_nkeys=1 \
                --load_factor="${lf}" \
                --memory_only=true \
                --workloads=ycsbc \
                --threads=1 \
                --seconds=0 \
            2>&1 || true
        )

        # Parse the [memory] line
        mem_line=$(echo "$log_output" | grep '\[memory\]' | tail -1)
        if [[ -z "$mem_line" ]]; then
            echo "ERROR: no [memory] line for nkeys=${nk} lf=${lf}"
            echo "$log_output" | tail -5
            continue
        fi

        nb=$(echo    "$mem_line" | grep -oP 'num_buckets=\K[0-9]+')
        seeds=$(echo  "$mem_line" | grep -oP 'seeds_MB=\K[0-9.]+')
        oth=$(echo    "$mem_line" | grep -oP 'othello_MB=\K[0-9.]+')
        total=$(echo  "$mem_line" | grep -oP 'total_MB=\K[0-9.]+')

        printf "%-12s  %-12s  %-14s  %-10s  %-12s  %-10s\n" \
            "${nk}" "${lf}" "${nb}" "${seeds}" "${oth}" "${total}"

        echo "${nk},${lf},${nb},${seeds},${oth},${total}" >> "$OUTPUT_CSV"
    done
done

echo ""
echo "Done.  CSV → ${OUTPUT_CSV}"
