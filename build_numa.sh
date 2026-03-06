#!/bin/bash

# Build script for OUTBACK with NUMA support

set -e

echo "============================================"
echo "Building OUTBACK with NUMA Support"
echo "============================================"

# Check for libnuma
if ! dpkg -l | grep -q libnuma-dev; then
    echo "Warning: libnuma-dev not found"
    echo "Install with: sudo apt-get install libnuma-dev"
    exit 1
fi

# Create build directory
mkdir -p build
cd build

# Configure
echo "Configuring..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build NUMA versions
echo "Building NUMA client and server..."
make client_numa -j$(nproc)
make server_numa -j$(nproc)

# Optionally build original RDMA versions
echo "Building original RDMA client and server..."
make client -j$(nproc) 2>/dev/null || echo "Note: Original client build failed (may need RDMA libraries)"
make server -j$(nproc) 2>/dev/null || echo "Note: Original server build failed (may need RDMA libraries)"

echo ""
echo "============================================"
echo "Build Complete!"
echo "============================================"
echo ""
echo "NUMA executables:"
echo "  - ./build/benchs/outback/client_numa"
echo "  - ./build/benchs/outback/server_numa"
echo ""
echo "To run:"
echo "  Server: sudo ./build/benchs/outback/server_numa --numa_node=1 --nkeys=1000000 --seconds=60"
echo "  Client: sudo ./build/benchs/outback/client_numa --nkeys=1000000 --threads=4 --seconds=60"
echo ""
