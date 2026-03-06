#!/bin/bash

# Quick Start Script for OUTBACK NUMA Setup
# This script automates the entire setup process

set -e

echo "=============================================="
echo "OUTBACK NUMA Quick Start"
echo "=============================================="
echo ""

# Check if running on Linux
if [[ "$OSTYPE" != "linux-gnu"* ]]; then
    echo "Error: This script requires Linux"
    exit 1
fi

# Check for NUMA
echo "[1/6] Checking NUMA availability..."
if ! command -v numactl &> /dev/null; then
    echo "   numactl not found. Installing..."
    sudo apt-get update
    sudo apt-get install -y numactl
fi

NUMA_NODES=$(numactl --hardware | grep "available:" | awk '{print $2}')
echo "   Found $NUMA_NODES NUMA node(s)"

if [ "$NUMA_NODES" -lt 2 ]; then
    echo ""
    echo "WARNING: Only $NUMA_NODES NUMA node detected!"
    echo "This system may not have multiple NUMA nodes."
    echo "The NUMA version requires at least 2 NUMA nodes."
    echo ""
    echo "Consider:"
    echo "  1. Using a two-socket server"
    echo "  2. Enabling NUMA in BIOS"
    echo "  3. Using the RDMA version instead"
    echo ""
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Install dependencies
echo ""
echo "[2/6] Installing dependencies..."
if [ ! -f ".setup_done" ]; then
    chmod +x setup-env.sh
    ./setup-env.sh numa
    touch .setup_done
    echo "   Dependencies installed"
else
    echo "   Dependencies already installed (skip with .setup_done file)"
fi

# Build
echo ""
echo "[3/6] Building NUMA version..."
chmod +x build_numa.sh
./build_numa.sh

# Check binaries
echo ""
echo "[4/6] Verifying build..."
if [ -f "./build/benchs/outback/server_numa" ] && [ -f "./build/benchs/outback/client_numa" ]; then
    echo "   ✓ Build successful"
else
    echo "   ✗ Build failed - executables not found"
    exit 1
fi

# Create test wrapper scripts
echo ""
echo "[5/6] Creating convenience scripts..."

cat > run_server_numa.sh << 'EOF'
#!/bin/bash
# OUTBACK NUMA Server Runner

NUMA_NODE=${1:-1}
NKEYS=${2:-1000000}
SECONDS=${3:-60}
WORKLOAD=${4:-ycsbc}

echo "Starting OUTBACK server on NUMA node $NUMA_NODE..."
echo "Keys: $NKEYS, Duration: ${SECONDS}s, Workload: $WORKLOAD"
echo ""

sudo ./build/benchs/outback/server_numa \
  --numa_node=$NUMA_NODE \
  --nkeys=$NKEYS \
  --seconds=$SECONDS \
  --workloads=$WORKLOAD
EOF

cat > run_client_numa.sh << 'EOF'
#!/bin/bash
# OUTBACK NUMA Client Runner

THREADS=${1:-4}
NKEYS=${2:-1000000}
BENCH_KEYS=${3:-500000}
SECONDS=${4:-60}
WORKLOAD=${5:-ycsbc}

echo "Starting OUTBACK client with $THREADS threads..."
echo "Keys: $NKEYS, Bench Keys: $BENCH_KEYS, Duration: ${SECONDS}s, Workload: $WORKLOAD"
echo ""

sudo taskset -c 0-$((THREADS-1)) ./build/benchs/outback/client_numa \
  --nkeys=$NKEYS \
  --bench_nkeys=$BENCH_KEYS \
  --threads=$THREADS \
  --seconds=$SECONDS \
  --workloads=$WORKLOAD
EOF

chmod +x run_server_numa.sh run_client_numa.sh
echo "   ✓ Created run_server_numa.sh and run_client_numa.sh"

# Show NUMA configuration
echo ""
echo "[6/6] System NUMA Configuration:"
echo "----------------------------------------"
numactl --hardware | head -10

echo ""
echo "=============================================="
echo "Setup Complete!"
echo "=============================================="
echo ""
echo "Quick Test (1 million keys, 60 seconds):"
echo ""
echo "  Terminal 1 (Server):"
echo "    ./run_server_numa.sh 1 1000000 60 ycsbc"
echo ""
echo "  Terminal 2 (Client - wait for server to start):"
echo "    ./run_client_numa.sh 4 1000000 500000 60 ycsbc"
echo ""
echo "Full Benchmark (50 million keys, 120 seconds):"
echo ""
echo "  Terminal 1 (Server):"
echo "    ./run_server_numa.sh 1 50000000 120 ycsbc"
echo ""
echo "  Terminal 2 (Client):"
echo "    ./run_client_numa.sh 8 50000000 10000000 120 ycsbc"
echo ""
echo "Script Arguments:"
echo "  run_server_numa.sh [numa_node] [nkeys] [seconds] [workload]"
echo "  run_client_numa.sh [threads] [nkeys] [bench_keys] [seconds] [workload]"
echo ""
echo "Workloads: ycsba, ycsbb, ycsbc, ycsbd, ycsbf"
echo ""
echo "For more information:"
echo "  - Architecture: README_NUMA.md"
echo "  - Setup guide: SETUP_GUIDE.md"
echo "  - Original README: README.md"
echo ""
