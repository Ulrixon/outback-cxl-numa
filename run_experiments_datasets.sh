#!/bin/bash

# Configuration defaults
EXP_SECONDS=120
SERVER_SECONDS=500
BENCH_NKEYS=10000000
COROS=2
OUTPUT_FILE="results/dataset_experiment_results.csv"

# Write CSV header if the file doesn't exist
if [ ! -f "$OUTPUT_FILE" ]; then
    echo "Experiment,Dataset,NKeys,Threads,MemThreads,LoadFactor,MeanThroughput(MOPs),MaxThroughput(MOPs),MinThroughput(MOPs),MeanLatency(us),P95Latency(us),P99Latency(us),P999Latency(us)" > "$OUTPUT_FILE"
fi

# Ensure datasets directory exists
mkdir -p datasets

# Download and extract FB and OSM if they don't exist
if [ ! -f "datasets/fb_200M_uint64" ]; then
    echo "Downloading FB dataset..."
    wget 'https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A/EATHF7' -O fb_200M_uint64.zst &
    W1=$!
fi

if [ ! -f "datasets/osm_cellids_200M_uint64" ]; then
    echo "Downloading OSM dataset..."
    wget 'https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A/8FX9BV' -O osm_cellids_200M_uint64.zst &
    W2=$!
fi

if [ -n "$W1" ] || [ -n "$W2" ]; then
    echo "Waiting for downloads to finish..."
    wait
    echo "Decompressing datasets..."
    zstd -d *.zst
    rm -f *.zst
    mv fb_200M_uint64 datasets/ 2>/dev/null
    mv osm_cellids_200M_uint64 datasets/ 2>/dev/null
fi

echo "Datasets ready!"

# Helper function to generate physcpubind string
get_client_cores() {
    local num_threads=$1
    local cores=""
    local current_core=2
    if [ "$num_threads" -ge 32 ]; then
        current_core=0
    fi
    for ((i=1; i<=$num_threads; i++)); do
        if [ $i -eq 1 ]; then
            cores="$current_core"
        else
            cores="$cores,$current_core"
        fi
        current_core=$((current_core + 2))
    done
    echo "$cores"
}

run_experiment() {
    DESC=$1
    DATASET=$2          # fb or osm
    NKEYS=$3            # 50000000, 20000000, 80000000
    MEM_THREADS=$4      # 1, 2, 3
    THREAD_COUNT=$5
    LOAD_FACTOR=$6      # 0.95, 0.75, 0.85
    CORE_OVERRIDE=$7    # Optional manual core pinning
    
    CLIENT_MEMBIND=0
    SERVER_MEMBIND=1
    SERVER_NUMA_NODE=1
    
    # We alter slightly for the 1MN 1CN local cpu test
    if [[ "$DESC" == *"on CPU"* && "$DESC" != *"different CPU"* ]]; then
        SERVER_MEMBIND=0
        SERVER_NUMA_NODE=0
    fi

    # Clean up stale memory before each run
    rm -f /dev/shm/outback_numa_*

    echo "=========================================================================="
    echo "Running: $DESC | Dataset: $DATASET | Keys: $NKEYS | Threads: $THREAD_COUNT | MemThreads: $MEM_THREADS | LF: $LOAD_FACTOR"
    echo "=========================================================================="

    # Start Server
    sudo numactl --physcpubind=0 --membind=$SERVER_MEMBIND \
        ./build/benchs/outback/server_numa \
        --numa_node=$SERVER_NUMA_NODE \
        --nkeys=$NKEYS \
        --seconds=$SERVER_SECONDS \
        --mem_threads=$MEM_THREADS \
        --load_factor=$LOAD_FACTOR \
        --workloads=$DATASET > server_out.log 2>&1 &
    SERVER_PID=$!

    # Small delay for Server to initiate Ludo table setup
    sleep 3

    if [ -n "$CORE_OVERRIDE" ]; then
        CLIENT_CORES=$CORE_OVERRIDE
    else
        CLIENT_CORES=$(get_client_cores $THREAD_COUNT)
    fi
    echo "Client bound to cores: $CLIENT_CORES"

    # Start Client and capture output
    sudo numactl --physcpubind=$CLIENT_CORES --membind=$CLIENT_MEMBIND \
        ./build/benchs/outback/client_numa \
        --nkeys=$NKEYS \
        --bench_nkeys=$BENCH_NKEYS \
        --threads=$THREAD_COUNT \
        --seconds=$EXP_SECONDS \
        --mem_threads=$MEM_THREADS \
        --coros=$COROS \
        --load_factor=$LOAD_FACTOR \
        --workloads=$DATASET > client_out.log 2>&1
    
    # Send SIGINT to server for graceful shared-memory cleanup
    echo "Performing graceful shutdown..."
    sudo kill -2 $SERVER_PID 2>/dev/null
    sleep 3

    # Extract metrics from client log
    MEAN_TPUT=$(grep "Mean Throughput(MOPs)" client_out.log | awk '{print $NF}')
    MAX_TPUT=$(grep "Max Throughput(MOPs)" client_out.log | awk '{print $NF}')
    MIN_TPUT=$(grep "Min Throughput(MOPs)" client_out.log | awk '{print $NF}')
    MEAN_LAT=$(grep "Mean Latency(us)" client_out.log | awk '{print $NF}')
    P95_LAT=$(grep "Tail Latency P95(us)" client_out.log | awk '{print $NF}')
    P99_LAT=$(grep "Tail Latency P99(us)" client_out.log | awk '{print $NF}')
    P999_LAT=$(grep "Tail Latency P999(us)" client_out.log | awk '{print $NF}')

    if [ -z "$MEAN_TPUT" ]; then
        MEAN_TPUT="ERROR"
        MAX_TPUT="ERROR"
        MIN_TPUT="ERROR"
        MEAN_LAT="ERROR"
        P95_LAT="ERROR"
        P99_LAT="ERROR"
        P999_LAT="ERROR"
        echo "WARNING: Failed to parse metrics! Check client_out.log"
    fi

    echo "Results: Mean Throughput: $MEAN_TPUT MOPs | P99 Latency: $P99_LAT us"

    # Write data entry to CSV
    echo "\"$DESC\",$DATASET,$NKEYS,$THREAD_COUNT,$MEM_THREADS,$LOAD_FACTOR,$MEAN_TPUT,$MAX_TPUT,$MIN_TPUT,$MEAN_LAT,$P95_LAT,$P99_LAT,$P999_LAT" >> "$OUTPUT_FILE"
    
    echo -e "Experiment completed.\n\n"
}

DEF_NKEYS=50000000
DEF_LF=0.95

for DATASET in "fb" "osm"; do

    echo "################################################################"
    echo "# STARTING DATASET: $DATASET"
    echo "################################################################"

    # 1. Base / Thread Scaling
    run_experiment "1MN 1CN 1 thread" $DATASET $DEF_NKEYS 1 1 $DEF_LF ""
    run_experiment "1MN 1CN 4 thread on CPU" $DATASET $DEF_NKEYS 1 4 $DEF_LF "2,2,2,2"
    run_experiment "1MN 1CN 2 thread on different CPU" $DATASET $DEF_NKEYS 1 2 $DEF_LF ""
    run_experiment "1MN 1CN 4 thread on different CPU" $DATASET $DEF_NKEYS 1 4 $DEF_LF ""
    run_experiment "1MN 1CN 8 thread on different CPU" $DATASET $DEF_NKEYS 1 8 $DEF_LF ""
    run_experiment "1MN 1CN 16 thread on different CPU" $DATASET $DEF_NKEYS 1 16 $DEF_LF ""
    run_experiment "1MN 1CN 32 thread on different CPU" $DATASET $DEF_NKEYS 1 32 $DEF_LF ""

    # 2. Mem_threads Scaling
    run_experiment "Mem_threads=2" $DATASET $DEF_NKEYS 2 32 $DEF_LF ""
    run_experiment "Mem_threads=3" $DATASET $DEF_NKEYS 3 32 $DEF_LF ""

    # 3. KV (Key Count) Scaling
    run_experiment "KV=20M" $DATASET 20000000 1 32 $DEF_LF ""
    run_experiment "KV=80M" $DATASET 80000000 1 32 $DEF_LF ""

    # 4. Loadfactor Scaling
    run_experiment "Loadfactor=0.75" $DATASET $DEF_NKEYS 1 32 0.75 ""
    run_experiment "Loadfactor=0.85" $DATASET $DEF_NKEYS 1 32 0.85 ""

done

echo "All tests finished! See $OUTPUT_FILE for the parsed data."