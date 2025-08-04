#!/bin/bash

# Simple script to run tensor benchmarks for performance testing
# Usage: ./run_tensor_benchmark.sh [server_addr] [tensor_size] [iterations]

SERVER_ADDR=${1:-"127.0.0.1:12345"}
TENSOR_SIZE=${2:-64}
ITERATIONS=${3:-10}

echo "=== TensorStore Remote DRAM Benchmark ==="
echo "Server: $SERVER_ADDR"
echo "Tensor Size: ${TENSOR_SIZE}x${TENSOR_SIZE}"
echo "Iterations: $ITERATIONS"
echo "=========================================="
echo

# Build the benchmark client if needed
echo "Building benchmark client..."
bazel build //examples/two_nodes:tensor_benchmark_client

if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo "✅ Build successful!"
echo

# Run the benchmark
echo "Running benchmark..."
./bazel-bin/examples/two_nodes/tensor_benchmark_client "$SERVER_ADDR" "$TENSOR_SIZE" "$ITERATIONS"

echo
echo "Benchmark completed!"