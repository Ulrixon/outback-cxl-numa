#!/bin/bash
# ============================================================
# run_experiments_rdma_9node.sh
#
# Runs Outback RDMA experiments across 9 CloudLab r320 nodes
# (1 MN + 8 CN), reproducing Paper Figures 10–13, 16, 17.
#
# Topology:
#   MN_HOST  — Memory Node (runs ./build/benchs/outback/server)
#   CN_HOSTS — 8 Compute Nodes (each runs ./build/benchs/outback/client)
#
# Expected results (paper Fig. 10, 4 MN threads, 8T/CN):
#   YCSB-C ~17 MOPs aggregate  |  YCSB-A ~15 MOPs  |  YCSB-F ~9 MOPs
#
# Prerequisites — verify once before running:
#   1. Passwordless SSH from THIS node to all 9 nodes:
#        for h in $MN_HOST "${CN_HOSTS[@]}"; do ssh-copy-id $SSH_USER@$h; done
#   2. MLNX_OFED 4.9-x installed on every node (legacy CX-3 driver):
#        ibv_devinfo   # should show mlx4_0 / active
#   3. Binary built on every node (same path REPO_DIR):
#        ssh $MN_HOST "cd $REPO_DIR && ./build.sh"
#        for h in "${CN_HOSTS[@]}"; do ssh $h "cd $REPO_DIR && ./build.sh"; done
#   4. Determine the MN's InfiniBand IP (used in --server_addr):
#        ssh $MN_HOST "ip addr show ib0"      # look for inet x.x.x.x
#   5. Determine the correct NIC index (usually 0; check if port is active):
#        ssh $MN_HOST "ibv_devinfo -d mlx4_0" # PORT_ACTIVE = active
#   6. Hugepages on MN (required for RDMA huge-page MR):
#        ssh $MN_HOST "echo 2048 | sudo tee /proc/sys/vm/nr_hugepages"
#
# Usage:
#   ./run_experiments_rdma_9node.sh          # run all experiments
#   ./run_experiments_rdma_9node.sh ycsb     # YCSB scaling (Fig 10)
#   ./run_experiments_rdma_9node.sh mt       # MN-thread scaling (Fig 12)
#   ./run_experiments_rdma_9node.sh coro     # coroutine sweep (Fig 13)
#   ./run_experiments_rdma_9node.sh datasets # dataset experiments (Fig 11)
#   ./run_experiments_rdma_9node.sh memory   # DMPH memory usage (Fig 16)
#
# Output:
#   results/rdma_9node_results.csv   — aggregated per-experiment metrics
#   results/rdma_9node_logs/         — per-node raw client/server logs
# ============================================================
set -euo pipefail

# ─────────────────────────────────────────────────────────────
# CONFIG — edit all values in this section before running
# ─────────────────────────────────────────────────────────────

# Hostnames or IPs reachable via SSH (control / management network)
MN_HOST="apt173.apt.emulab.net"
CN_HOSTS=(
    "apt175.apt.emulab.net"   # cn0
    "apt182.apt.emulab.net"   # cn1
    "apt185.apt.emulab.net"   # cn2
    "apt190.apt.emulab.net"   # cn3
    "apt178.apt.emulab.net"   # cn4
    "apt180.apt.emulab.net"   # cn5
    "apt177.apt.emulab.net"   # cn6
    "apt181.apt.emulab.net"   # cn7
)

# MN's InfiniBand IP address (IPoIB, used in --server_addr on clients)
# Find with: ssh $MN_HOST "ip addr show ib0"  (or ib1)
# TODO: fill in after running: ssh Cheng@apt173.apt.emulab.net "ip addr show ib0"
MN_IB_ADDR="128.110.96.173"
SERVER_PORT=8888

# RDMA NIC index (0 = first port; try 1 if ibv_devinfo shows port 2 active)
NIC_IDX=0

# SSH user (must be the same on all nodes)
SSH_USER="Cheng"

# Absolute path to the repo on ALL nodes (must be identical — use NFS or
# manually copy). On APT/Emulab, /proj/<group>/ is NFS-shared across nodes.
# TODO: replace with your actual project name from the Emulab portal
REPO_DIR="/proj/cs620426sp-PG0/outback-cxl-numa"

# ─────────────────────────────────────────────────────────────
# EXPERIMENT PARAMETERS
# ─────────────────────────────────────────────────────────────

NKEYS=50000000        # 50 M KV pairs (paper default)
BENCH_NKEYS=10000000  # 10 M key working set for YCSB operations
EXP_SECONDS=120       # client measurement window (seconds)
SERVER_SECONDS=800    # server lifetime per invocation (>EXP_SECONDS)

# Seconds to wait after launching the server before starting clients.
# The server must build the DMPH index (Ludo table) for NKEYS before
# accepting connections.  At 50 M keys on an r320, this takes ~45–90 s.
# If clients time out during the experiment, increase this value.
SERVER_WARMUP_SECS=75

DEFAULT_COROS=2
DEFAULT_MEM_THREADS=1

# ─────────────────────────────────────────────────────────────
# OUTPUT
# ─────────────────────────────────────────────────────────────

RESULTS_DIR="results"
OUTPUT_FILE="${RESULTS_DIR}/rdma_9node_results.csv"
LOG_DIR="${RESULTS_DIR}/rdma_9node_logs"
mkdir -p "${LOG_DIR}"

# Write CSV header once
if [[ ! -f "${OUTPUT_FILE}" ]]; then
    printf "Experiment,Workload,NKeys,ActiveCNs,ThreadsPerCN,TotalThreads,MemThreads,Coros," \
        > "${OUTPUT_FILE}"
    printf "TotalTput(MOPs),MeanTput(MOPs),MaxTput(MOPs),MinTput(MOPs)," \
        >> "${OUTPUT_FILE}"
    echo   "MeanLatency(us),P95Latency(us),P99Latency(us),P999Latency(us)" \
        >> "${OUTPUT_FILE}"
fi

# Which experiments to run (set via first positional argument or "all")
RUN_TARGET="${1:-all}"

# ─────────────────────────────────────────────────────────────
# HELPER: timestamped log line
# ─────────────────────────────────────────────────────────────
log() { echo "[$(date +%H:%M:%S)] $*"; }

# ─────────────────────────────────────────────────────────────
# HELPER: run a command on a remote host in the background.
# Redirects all output (stdout+stderr) to a LOCAL logfile.
# Echoes the PID of the backgrounded SSH process.
# ─────────────────────────────────────────────────────────────
ssh_bg() {
    local host="$1"
    local logfile="$2"
    shift 2
    ssh -o StrictHostKeyChecking=no \
        -o BatchMode=yes \
        -o ConnectTimeout=15 \
        "${SSH_USER}@${host}" \
        "cd ${REPO_DIR} && $*" \
        > "${logfile}" 2>&1 &
    echo $!
}

# ─────────────────────────────────────────────────────────────
# HELPER: run a command on a remote host synchronously.
# ─────────────────────────────────────────────────────────────
ssh_run() {
    local host="$1"
    shift
    ssh -o StrictHostKeyChecking=no \
        -o BatchMode=yes \
        -o ConnectTimeout=15 \
        "${SSH_USER}@${host}" \
        "cd ${REPO_DIR} && $*"
}

# ─────────────────────────────────────────────────────────────
# HELPER: start the RDMA server on MN.
#   $1 — local log file path (written by ssh_bg, polled by wait_for_server)
#   $2 — mem_threads
#   $3 — workload string (e.g. "ycsbc")
#   $4 — nkeys (optional, defaults to $NKEYS)
# Echoes the SSH background PID.
# ─────────────────────────────────────────────────────────────
start_server() {
    local logfile="$1"
    local mem_threads="$2"
    local workload="$3"
    local nkeys="${4:-${NKEYS}}"

    # Pin server threads to cores 0..mem_threads-1
    local core_spec
    if [[ ${mem_threads} -le 1 ]]; then
        core_spec="0"
    else
        core_spec="0-$(( mem_threads - 1 ))"
    fi

    log "  [MN] server: mt=${mem_threads} workload=${workload} nkeys=${nkeys}"
    ssh_bg "${MN_HOST}" "${logfile}" \
        "sudo taskset -c ${core_spec} \
         ./build/benchs/outback/server \
         --seconds=${SERVER_SECONDS} \
         --nkeys=${nkeys} \
         --mem_threads=${mem_threads} \
         --workloads=${workload} \
         --nic_idx=${NIC_IDX}"
}

# ─────────────────────────────────────────────────────────────
# HELPER: wait until the server log shows a "ready" marker,
# or fall back to a fixed sleep if no marker appears.
# ─────────────────────────────────────────────────────────────
wait_for_server() {
    local logfile="$1"
    local max_poll="${SERVER_WARMUP_SECS}"

    log -n "  [MN] Waiting for server ready (up to ${max_poll}s)..."
    for (( i=1; i<=max_poll; i++ )); do
        # These patterns cover common R2/Outback server startup messages.
        # If your binary prints something different, extend the regex.
        if grep -qEi \
            "memory node started|server ready|ludo.*finished|registered.*memory|MR registered|accept connect" \
            "${logfile}" 2>/dev/null; then
            echo " ready at ${i}s"
            sleep 3   # brief extra pause for QP setup
            return 0
        fi
        sleep 1
    done
    echo " no ready marker after ${max_poll}s — proceeding (server may still be building)"
    # Give the server a generous extra buffer in case the marker was missed
    sleep 20
}

# ─────────────────────────────────────────────────────────────
# HELPER: cleanly terminate the server on MN.
#   $1 — SSH background PID returned by start_server
# ─────────────────────────────────────────────────────────────
kill_server() {
    local ssh_pid="$1"
    log "  [MN] Stopping server"
    ssh_run "${MN_HOST}" \
        "sudo pkill -SIGINT -f 'benchs/outback/server' 2>/dev/null; sleep 2; \
         sudo pkill -SIGKILL -f 'benchs/outback/server' 2>/dev/null; true" \
        2>/dev/null || true
    kill "${ssh_pid}" 2>/dev/null || true
    wait "${ssh_pid}" 2>/dev/null || true
    sleep 4
}

# ─────────────────────────────────────────────────────────────
# CORE: run one experiment.
#   Fans out RDMA clients to n_cn CNs in parallel, waits for all
#   to finish, aggregates throughput (sum) and latency (avg/max).
#
#   $1 — desc         human-readable experiment label
#   $2 — workload     e.g. "ycsbc"
#   $3 — nkeys        total KV pairs loaded in server
#   $4 — n_cn         number of active CN nodes (1–8)
#   $5 — threads_per_cn  client threads on each CN
#   $6 — mem_threads  MN server threads (for logging only here)
#   $7 — coros        coroutines per client thread
# ─────────────────────────────────────────────────────────────
run_experiment() {
    local desc="$1"
    local workload="$2"
    local nkeys="$3"
    local n_cn="$4"
    local threads_per_cn="$5"
    local mem_threads="$6"
    local coros="$7"

    local total_threads=$(( n_cn * threads_per_cn ))

    # Pin threads 0..threads_per_cn-1 on each CN node.
    # r320 has 8 physical cores / 16 HT threads; keep within physical cores.
    local core_spec
    if [[ ${threads_per_cn} -le 1 ]]; then
        core_spec="0"
    else
        core_spec="0-$(( threads_per_cn - 1 ))"
    fi

    # Build a unique tag for log file names (no spaces/slashes)
    local tag="${desc//[ \/]/_}_${workload}_${n_cn}cn_${threads_per_cn}t_mt${mem_threads}_c${coros}"

    log "    → ${desc} | ${workload} | ${n_cn}CN × ${threads_per_cn}T = ${total_threads}T total | MT=${mem_threads} C=${coros}"

    # ── Launch all n_cn clients in parallel ──────────────────
    local client_pids=()
    local client_logs=()

    for (( i=0; i<n_cn; i++ )); do
        local cn_host="${CN_HOSTS[$i]}"
        local logfile="${LOG_DIR}/client_cn${i}_${tag}.log"
        client_logs+=("${logfile}")

        local pid
        pid=$(ssh_bg "${cn_host}" "${logfile}" \
            "sudo taskset -c ${core_spec} \
             ./build/benchs/outback/client \
             --nic_idx=${NIC_IDX} \
             --server_addr=${MN_IB_ADDR}:${SERVER_PORT} \
             --threads=${threads_per_cn} \
             --coros=${coros} \
             --seconds=${EXP_SECONDS} \
             --nkeys=${nkeys} \
             --bench_nkeys=${BENCH_NKEYS} \
             --workloads=${workload}")
        client_pids+=("${pid}")
    done

    # ── Wait for all clients to finish ───────────────────────
    for pid in "${client_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done

    # ── Aggregate metrics across all CN logs ─────────────────
    local total_tput=0
    local sum_max_tput=0
    local sum_min_tput=0
    local sum_lat=0
    local max_p95=0
    local max_p99=0
    local max_p999=0
    local n_valid=0

    for (( i=0; i<n_cn; i++ )); do
        local logfile="${client_logs[$i]}"
        local cn_host="${CN_HOSTS[$i]}"
        local tput maxt mint lat p95 p99 p999

        # Parse the same output fields that run_experiments.sh uses
        tput=$(grep  "Mean Throughput(MOPs)" "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        maxt=$(grep  "Max Throughput(MOPs)"  "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        mint=$(grep  "Min Throughput(MOPs)"  "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        lat=$(  grep "Mean Latency(us)"       "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        p95=$(  grep "Tail Latency P95(us)"   "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        p99=$(  grep "Tail Latency P99(us)"   "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)
        p999=$( grep "Tail Latency P999(us)"  "${logfile}" 2>/dev/null | awk '{print $NF}' | tail -1)

        if [[ -n "${tput:-}" && "${tput}" != "0" ]]; then
            total_tput=$(awk "BEGIN{printf \"%.3f\", ${total_tput}+${tput}}")
            sum_max_tput=$(awk "BEGIN{printf \"%.3f\", ${sum_max_tput}+${maxt:-0}}")
            sum_min_tput=$(awk "BEGIN{printf \"%.3f\", ${sum_min_tput}+${mint:-0}}")
            sum_lat=$(awk "BEGIN{printf \"%.3f\", ${sum_lat}+${lat:-0}}")

            local gt95=0 gt99=0 gt999=0
            [[ -n "${p95:-}"  ]] && gt95=$(awk  "BEGIN{print (${p95}>=${max_p95})?1:0}")
            [[ -n "${p99:-}"  ]] && gt99=$(awk  "BEGIN{print (${p99}>=${max_p99})?1:0}")
            [[ -n "${p999:-}" ]] && gt999=$(awk "BEGIN{print (${p999}>=${max_p999})?1:0}")
            [[ ${gt95}  -eq 1 ]] && max_p95="${p95}"
            [[ ${gt99}  -eq 1 ]] && max_p99="${p99}"
            [[ ${gt999} -eq 1 ]] && max_p999="${p999}"
            (( ++n_valid ))
        else
            log "      WARNING: CN${i} (${cn_host}) — no valid output. Check ${logfile}"
        fi
    done

    # ── Compute aggregate / averages ─────────────────────────
    local mean_tput avg_lat
    if [[ ${n_valid} -gt 0 ]]; then
        mean_tput=$(awk "BEGIN{printf \"%.3f\", ${total_tput}/${n_cn}}")
        avg_lat=$(  awk "BEGIN{printf \"%.3f\", ${sum_lat}/${n_valid}}")
    else
        total_tput="ERROR"; mean_tput="ERROR"; sum_max_tput="ERROR"
        sum_min_tput="ERROR"; avg_lat="ERROR"
        max_p95="ERROR"; max_p99="ERROR"; max_p999="ERROR"
    fi

    log "      total=${total_tput} MOPs  mean/CN=${mean_tput}  lat=${avg_lat} us  P99=${max_p99} us  [${n_valid}/${n_cn} CNs OK]"

    # ── Write CSV row ─────────────────────────────────────────
    printf "\"%s\",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
        "${desc}" "${workload}" "${nkeys}" \
        "${n_cn}" "${threads_per_cn}" "${total_threads}" \
        "${mem_threads}" "${coros}" \
        "${total_tput}" "${mean_tput}" "${sum_max_tput}" "${sum_min_tput}" \
        "${avg_lat}" "${max_p95}" "${max_p99}" "${max_p999}" \
        >> "${OUTPUT_FILE}"
}

# ════════════════════════════════════════════════════════════════
# EXP 1  —  YCSB Throughput Scaling  (Paper Figs. 9 and 10)
#
# Sweeps all 5 YCSB workloads × {1,2,3,4} MN threads × {1,2,4,8} T/CN.
# With 8 CN nodes × 8 T/CN = 64 total CN threads, this exactly matches
# the paper's Fig. 10 configuration (CX-3, 4 MN threads, 8 CN × 8T).
#
# Expected aggregate peaks (paper Fig. 10, 4 MN threads):
#   YCSB-C ~17 MOPs  |  YCSB-B ~16.5  |  YCSB-A ~15  |  YCSB-F ~9
# ════════════════════════════════════════════════════════════════
run_exp_ycsb() {
    log "════════════════════════════════════════════"
    log "EXP 1: YCSB Throughput Scaling (Figs 9/10)"
    log "════════════════════════════════════════════"

    local workloads=("ycsbc" "ycsbb" "ycsba" "ycsbd" "ycsbf")
    local threads_list=(1 2 4 8)         # T/CN → 8 / 16 / 32 / 64 total
    local mt_list=(1 2 3 4)              # MN threads (Fig. 10 uses 4)

    for workload in "${workloads[@]}"; do
        for mt in "${mt_list[@]}"; do
            local srv_log="${LOG_DIR}/server_${workload}_mt${mt}.log"
            local srv_pid
            srv_pid=$(start_server "${srv_log}" "${mt}" "${workload}")
            wait_for_server "${srv_log}"

            for tpc in "${threads_list[@]}"; do
                run_experiment \
                    "Fig10 8CN mt${mt}" \
                    "${workload}" "${NKEYS}" \
                    8 "${tpc}" "${mt}" "${DEFAULT_COROS}"
            done

            kill_server "${srv_pid}"
            sleep 5
        done
    done
}

# ════════════════════════════════════════════════════════════════
# EXP 2  —  MN Thread Scaling  (Paper Fig. 12)
#
# Holds CN configuration fixed (8 CNs × 8 T/CN = 64 total threads)
# and varies MN threads: 1 → 2 → 3 → 4.
# Expected: near-linear aggregate throughput scaling with MN threads.
# ════════════════════════════════════════════════════════════════
run_exp_mt() {
    log "════════════════════════════════════════════"
    log "EXP 2: MN Thread Scaling (Fig 12)"
    log "════════════════════════════════════════════"

    local workload="ycsbc"
    local threads_per_cn=8     # 8 × 8 = 64 total CN threads

    for mt in 1 2 3 4; do
        local srv_log="${LOG_DIR}/server_${workload}_mt${mt}_exp2.log"
        local srv_pid
        srv_pid=$(start_server "${srv_log}" "${mt}" "${workload}")
        wait_for_server "${srv_log}"

        run_experiment \
            "Fig12 MNscale" \
            "${workload}" "${NKEYS}" \
            8 "${threads_per_cn}" "${mt}" "${DEFAULT_COROS}"

        kill_server "${srv_pid}"
        sleep 5
    done
}

# ════════════════════════════════════════════════════════════════
# EXP 3  —  Coroutine Effects  (Paper Fig. 13)
#
# Sweeps coroutine count (1, 2, 3) and thread count (1–8 T/CN)
# for MT=1 and MT=2, reproducing the latency–throughput "hockey stick"
# curves.  Both MT panels (Fig. 13 left and right) are produced here.
# ════════════════════════════════════════════════════════════════
run_exp_coro() {
    log "════════════════════════════════════════════"
    log "EXP 3: Coroutine Effects (Fig 13)"
    log "════════════════════════════════════════════"

    local workload="ycsbc"
    local threads_list=(1 2 4 8)   # T/CN → 8 / 16 / 32 / 64 total

    for mt in 1 2; do
        for coro in 1 2 3; do
            local srv_log="${LOG_DIR}/server_${workload}_mt${mt}_coro${coro}.log"
            local srv_pid
            srv_pid=$(start_server "${srv_log}" "${mt}" "${workload}")
            wait_for_server "${srv_log}"

            for tpc in "${threads_list[@]}"; do
                run_experiment \
                    "Fig13 C${coro} MT${mt}" \
                    "${workload}" "${NKEYS}" \
                    8 "${tpc}" "${mt}" "${coro}"
            done

            kill_server "${srv_pid}"
            sleep 5
        done
    done
}

# ════════════════════════════════════════════════════════════════
# EXP 4  —  Real-World Dataset Experiments  (Paper Fig. 11)
#
# Tests Facebook (FB) and OpenStreetMap (OSM) key distributions.
# NOTE: datasets must be pre-downloaded under ./datasets/ on the MN.
# Run "./build.sh 1" once (it downloads datasets) or manually:
#   wget .../fb_200M_uint64.zst  && zstd -d …
#   wget .../osm_cellids_200M_uint64.zst && zstd -d …
#
# Known limitation (discovered during reproduction):
#   Zipfian distribution for these datasets is non-functional in the
#   source; only uniform distribution produces valid results (Fig. 11a/b).
# ════════════════════════════════════════════════════════════════
run_exp_datasets() {
    log "════════════════════════════════════════════"
    log "EXP 4: Dataset Experiments (Fig 11)"
    log "════════════════════════════════════════════"

    local workloads=("fb" "osm")
    local threads_list=(1 2 4 8)   # T/CN → 8 / 16 / 32 / 64 total

    for workload in "${workloads[@]}"; do
        for mt in 1 2 3; do
            local srv_log="${LOG_DIR}/server_${workload}_mt${mt}_exp4.log"
            local srv_pid
            srv_pid=$(start_server "${srv_log}" "${mt}" "${workload}")
            wait_for_server "${srv_log}"

            for tpc in "${threads_list[@]}"; do
                run_experiment \
                    "Fig11 ${workload} MT${mt}" \
                    "${workload}" "${NKEYS}" \
                    8 "${tpc}" "${mt}" "${DEFAULT_COROS}"
            done

            kill_server "${srv_pid}"
            sleep 5
        done
    done
}

# ════════════════════════════════════════════════════════════════
# EXP 5  —  DMPH Index Memory Usage  (Paper Fig. 16)
#
# Runs a short read-only probe (--memory_only, if supported) on one
# CN to measure the DMPH index footprint at various key counts.
# If --memory_only is unavailable, a 5-second YCSB-C run is used and
# the log is preserved for manual RSS inspection.
#
# WARNING: 80M keys causes OOM on the r320's 16 GB DRAM.
#          Use nkeys ≤ 60M to stay safe on this hardware.
# ════════════════════════════════════════════════════════════════
run_exp_memory() {
    log "════════════════════════════════════════════"
    log "EXP 5: DMPH Memory Usage (Fig 16)"
    log "════════════════════════════════════════════"

    # 80M is omitted — causes OOM on r320 (16 GB DRAM)
    local nkeys_list=(20000000 40000000 50000000 60000000)
    local workload="ycsbc"
    local cn0="${CN_HOSTS[0]}"

    for nkeys in "${nkeys_list[@]}"; do
        local srv_log="${LOG_DIR}/server_${workload}_nk${nkeys}_mem.log"
        local srv_pid
        srv_pid=$(start_server "${srv_log}" 1 "${workload}" "${nkeys}")
        wait_for_server "${srv_log}"

        local logfile="${LOG_DIR}/memory_cn0_nk${nkeys}.log"

        # Attempt --memory_only first (custom monitor binary), fall back to
        # a short standard run so the log is available for RSS inspection.
        ssh_run "${cn0}" \
            "sudo taskset -c 0 \
             ./build/benchs/outback/client \
             --nic_idx=${NIC_IDX} \
             --server_addr=${MN_IB_ADDR}:${SERVER_PORT} \
             --threads=1 --coros=1 \
             --seconds=5 \
             --nkeys=${nkeys} \
             --bench_nkeys=1000000 \
             --workloads=${workload} \
             --memory_only 2>/dev/null || \
             sudo taskset -c 0 \
             ./build/benchs/outback/client \
             --nic_idx=${NIC_IDX} \
             --server_addr=${MN_IB_ADDR}:${SERVER_PORT} \
             --threads=1 --coros=1 \
             --seconds=5 \
             --nkeys=${nkeys} \
             --bench_nkeys=1000000 \
             --workloads=${workload}" \
            > "${logfile}" 2>&1 || true

        # Parse memory output (custom monitor prints "DMPH index: X.XX MB")
        local dmph_mb rss_mb
        dmph_mb=$(grep -iE "DMPH.*index.*MB|dmph.*[0-9]+\.[0-9]+.*MB" \
                      "${logfile}" 2>/dev/null | \
                  awk '{for(i=1;i<=NF;i++) if($i~/^[0-9]+\.[0-9]+$/) print $i}' | \
                  head -1)
        rss_mb=$(  grep -i "RSS\|rss\|VmRSS" "${logfile}" 2>/dev/null | \
                   awk '{print $NF}' | head -1)
        dmph_mb="${dmph_mb:-N/A}"
        rss_mb="${rss_mb:-N/A}"

        log "    nkeys=${nkeys}: DMPH=${dmph_mb} MB  RSS=${rss_mb} MB  (see ${logfile})"

        printf "\"Fig16 memory\",%s,%s,1,1,1,1,1,%s,%s,0,0,0,0,0,0\n" \
            "${workload}" "${nkeys}" "${dmph_mb}" "${rss_mb}" \
            >> "${OUTPUT_FILE}"

        kill_server "${srv_pid}"
        sleep 3
    done
}

# ─────────────────────────────────────────────────────────────
# Verify SSH connectivity to all 9 nodes before starting
# ─────────────────────────────────────────────────────────────
preflight_check() {
    log "Checking SSH connectivity..."
    local failed=0
    for host in "${MN_HOST}" "${CN_HOSTS[@]}"; do
        if ! ssh -o StrictHostKeyChecking=no \
                 -o BatchMode=yes \
                 -o ConnectTimeout=8 \
                 "${SSH_USER}@${host}" "hostname" >/dev/null 2>&1; then
            log "  ERROR: cannot reach ${host}"
            (( ++failed ))
        else
            log "  OK: ${host}"
        fi
    done
    if [[ ${failed} -gt 0 ]]; then
        log "Preflight failed: ${failed} node(s) unreachable. Aborting."
        exit 1
    fi
    log "All 9 nodes reachable."

    # Check that the binary exists on MN
    if ! ssh_run "${MN_HOST}" "test -f ./build/benchs/outback/server" 2>/dev/null; then
        log "ERROR: binary not found on MN (${MN_HOST}). Run ./build.sh first."
        exit 1
    fi
    log "Binary present on MN."
}

# ─────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────

log "Outback RDMA 9-node experiment runner"
log "MN  : ${MN_HOST} (IB addr ${MN_IB_ADDR}:${SERVER_PORT}, nic_idx=${NIC_IDX})"
log "CNs : ${CN_HOSTS[*]}"
log "Repo: ${REPO_DIR}"
log "Out : ${OUTPUT_FILE}"
echo ""

preflight_check
echo ""

# ─────────────────────────────────────────────────────────────
# SETUP: push and run setup-env.sh on all 9 nodes in parallel,
# then build the binary on each node.
#
# Run once before any experiments:
#   ./run_experiments_rdma_9node.sh setup
# ─────────────────────────────────────────────────────────────
run_setup() {
    log "════════════════════════════════════════════"
    log "SETUP: installing RDMA drivers + deps on all 9 nodes"
    log "════════════════════════════════════════════"
    log "WARNING: this takes ~10–20 min per node (OFED download + build)"

    local all_nodes=("${MN_HOST}" "${CN_HOSTS[@]}")
    local pids=()

    for host in "${all_nodes[@]}"; do
        local logfile="${LOG_DIR}/setup_${host}.log"
        log "  [${host}] starting setup (log: ${logfile})"

        # Copy setup-env.sh and build.sh to the node, then run setup
        (
            # Ensure the repo directory exists on the remote node
            ssh -o StrictHostKeyChecking=no -o BatchMode=yes \
                "${SSH_USER}@${host}" "mkdir -p ${REPO_DIR}"

            # Push the two scripts needed for setup + build
            scp -o StrictHostKeyChecking=no \
                setup-env.sh build.sh "${SSH_USER}@${host}:${REPO_DIR}/"

            # Run setup (RDMA mode) then build
            ssh -o StrictHostKeyChecking=no -o BatchMode=yes \
                "${SSH_USER}@${host}" \
                "cd ${REPO_DIR} && \
                 chmod +x setup-env.sh build.sh && \
                 bash setup-env.sh && \
                 bash build.sh"
        ) > "${logfile}" 2>&1 &
        pids+=($!)
    done

    log "  Waiting for all ${#all_nodes[@]} nodes to finish setup..."
    local failed=0
    for i in "${!pids[@]}"; do
        local host="${all_nodes[$i]}"
        if wait "${pids[$i]}"; then
            log "  [${host}] setup OK"
        else
            log "  [${host}] setup FAILED — check ${LOG_DIR}/setup_${host}.log"
            (( ++failed ))
        fi
    done

    if [[ ${failed} -gt 0 ]]; then
        log "Setup failed on ${failed} node(s). Fix errors before running experiments."
        exit 1
    fi
    log "All nodes set up and built successfully."
}

case "${RUN_TARGET}" in
    setup)    run_setup      ;;
    ycsb)     run_exp_ycsb   ;;
    mt)       run_exp_mt     ;;
    coro)     run_exp_coro   ;;
    datasets) run_exp_datasets ;;
    memory)   run_exp_memory   ;;
    all)
        run_exp_ycsb
        run_exp_mt
        run_exp_coro
        run_exp_datasets
        run_exp_memory
        ;;
    *)
        echo "Usage: $0 [setup|ycsb|mt|coro|datasets|memory|all]"
        echo ""
        echo "  setup    — install RDMA drivers, deps, and build on all 9 nodes (run once)"
        echo "  ycsb     — YCSB scaling, all 5 workloads × 4 MN threads (Fig 10)"
        echo "  mt       — MN-thread scaling YCSB-C, 64 CN threads (Fig 12)"
        echo "  coro     — coroutine sweep YCSB-C, C=1/2/3, MT=1/2 (Fig 13)"
        echo "  datasets — FB / OSM dataset experiments (Fig 11)"
        echo "  memory   — DMPH index size sweep 20M–60M keys (Fig 16)"
        echo "  all      — run all of the above in order"
        exit 1
        ;;
esac

log "All experiments complete."
log "Results: ${OUTPUT_FILE}"
log "Logs   : ${LOG_DIR}/"
