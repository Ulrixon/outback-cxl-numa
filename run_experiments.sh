#!/bin/bash

# Configuration
WORKLOAD="ycsbd"
EXP_SECONDS=120
SERVER_SECONDS=500
NKEYS=50000000
BENCH_NKEYS=10000000
MEM_THREADS=1
COROS=2
OUTPUT_FILE="results/experiment_results.csv"

# Write CSV header if the file doesn't exist
if [ ! -f "$OUTPUT_FILE" ]; then
    echo "Experiment,Workload,NKeys,Threads,MeanThroughput(MOPs),MaxThroughput(MOPs),MinThroughput(MOPs),MeanLatency(us),P95Latency(us),P99Latency(us),P999Latency(us)" > "$OUTPUT_FILE"
fi

# Clean up stale memory
rm -f /dev/shm/outback_numa_*

# Helper function to generate physcpubind string (e.g. up to 64 cores on an even/odd splitting topology - typical 0 or 1 index base)
get_client_cores() {
    local num_threads=$1
    local cores=""
    
    # Start from core 2 to prevent contention with the server (which is on core 0)
    local current_core=2
    
    # If 32 threads are requested, we MUST start at 0 to fit all 32 even cores (0,2,...,62)
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
    CLIENT_MEMBIND=$2   # 0 or 1
    SERVER_MEMBIND=$3   # 0 or 1
    SERVER_NUMA_NODE=$4 # 0 or 1
    THREADS=$5
    CORE_OVERRIDE=$6    # Optional manual core pinning
    
    echo "=========================================================================="
    echo "Running: $DESC"
    echo "=========================================================================="

    # Start Server (on physical CPU 0, binding its memory based on parameter)
    sudo numactl --physcpubind=0 --membind=$SERVER_MEMBIND \
        ./build/benchs/outback/server_numa \
        --numa_node=$SERVER_NUMA_NODE \
        --nkeys=$NKEYS \
        --seconds=$SERVER_SECONDS \
        --mem_threads=$MEM_THREADS \
        --workloads=$WORKLOAD > server_out.log 2>&1 &
    SERVER_PID=$!

    # Small delay for Server to initiate Ludo table setup
    sleep 2

    if [ -n "$CORE_OVERRIDE" ]; then
        CLIENT_CORES=$CORE_OVERRIDE
    else
        CLIENT_CORES=$(get_client_cores $THREADS)
    fi
    echo "Client bound to cores: $CLIENT_CORES"

    # Start Client and capture output
    sudo numactl --physcpubind=$CLIENT_CORES --membind=$CLIENT_MEMBIND \
        ./build/benchs/outback/client_numa \
        --nkeys=$NKEYS \
        --bench_nkeys=$BENCH_NKEYS \
        --threads=$THREADS \
        --seconds=$EXP_SECONDS \
        --mem_threads=$MEM_THREADS \
        --coros=$COROS \
        --workloads=$WORKLOAD > client_out.log 2>&1
    
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

    # Validate output (in case it crashed or failed to parse)
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

    # Print summary to console
    echo "Results:"
    echo "  Mean Throughput: $MEAN_TPUT MOPs"
    echo "  P99  Latency   : $P99_LAT us"

    # Write data entry to CSV
    echo "\"$DESC\",$WORKLOAD,$NKEYS,$THREADS,$MEAN_TPUT,$MAX_TPUT,$MIN_TPUT,$MEAN_LAT,$P95_LAT,$P99_LAT,$P999_LAT" >> "$OUTPUT_FILE"
    
    echo -e "Experiment completed.\n\n"
}

# Parameters: Description, Client_Membind, Server_Membind, Server_NumaNode, Threads

# 5. 1MN 1CN 8thread on different CPU (Cross-CPU / CXL Emulation)
run_experiment "1MN 1CN 8 thread on different CPU" 0 1 1 8

# 6. 1MN 1CN 16thread on different CPU (Cross-CPU / CXL Emulation)
run_experiment "1MN 1CN 16 thread on different CPU" 0 1 1 16

# 7. 1MN 1CN 32thread on different CPU (Cross-CPU / CXL Emulation)
run_experiment "1MN 1CN 32 thread on different CPU" 0 1 1 32

echo "All tests finished! See $OUTPUT_FILE for the parsed data."
