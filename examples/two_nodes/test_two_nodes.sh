#!/bin/bash

# Test script for 2-node tensor transfer using remote_dram

echo "=== TensorStore Remote DRAM Two-Node Test ==="
echo "Building the two-node test programs..."

# Build both programs
bazel build //examples/two_nodes:node1_writer //examples/two_nodes:node2_server

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "✓ Build successful!"
echo ""

echo "=== Starting Node 2 Server ==="
# Start server in background
bazel run //examples/two_nodes:node2_server &
SERVER_PID=$!

# Give server time to start
sleep 3

echo ""
echo "=== Running Node 1 Writer ==="
# Run client
bazel run //examples/two_nodes:node1_writer

echo ""
echo "=== Test completed ==="
echo "Stopping server..."

# Stop server
kill $SERVER_PID 2>/dev/null

echo "Done!" 