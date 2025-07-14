#!/bin/bash

# Copyright 2025 The TensorStore Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# TensorStore Remote DRAM 2-Node Test Script
#
# This script demonstrates the remote_dram driver by running a 2-node test:
# - Node 1: Server that listens for connections
# - Node 2: Client that creates and transfers a TensorStore array
#
# Usage: ./run_2node_test.sh [server_port]

set -e

# Configuration
SERVER_PORT=${1:-12345}
SERVER_ADDR="127.0.0.1:${SERVER_PORT}"
ARRAY_NAME="test_array_$(date +%s)"
WAIT_TIME=3

echo "=== TensorStore Remote DRAM 2-Node Test ==="
echo "Server address: ${SERVER_ADDR}"
echo "Array name: ${ARRAY_NAME}"
echo "Wait time: ${WAIT_TIME} seconds"
echo

# Build the programs
echo "Building remote_dram server and client..."
python3 ../bazelisk.py build //examples:remote_dram_server //examples:remote_dram_client
echo "✓ Build completed successfully"
echo

# Start the server in the background
echo "Starting remote_dram server..."
../bazel-bin/examples/remote_dram_server --listen_addr=0.0.0.0:${SERVER_PORT} --verification_interval=10 &
SERVER_PID=$!

# Trap to ensure server cleanup
trap "echo 'Stopping server...'; kill $SERVER_PID 2>/dev/null || true; wait $SERVER_PID 2>/dev/null || true; echo 'Server stopped.'" EXIT

# Wait for server to start
echo "Waiting ${WAIT_TIME} seconds for server to start..."
sleep ${WAIT_TIME}

# Check if server is still running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "❌ Server failed to start!"
    exit 1
fi

echo "✓ Server started successfully (PID: $SERVER_PID)"
echo

# Run the client
echo "Running remote_dram client..."
../bazel-bin/examples/remote_dram_client \
    --server_addr=${SERVER_ADDR} \
    --array_name=${ARRAY_NAME} \
    --wait_before_connect=1

CLIENT_EXIT_CODE=$?

# Give some time for final server output
sleep 2

echo
echo "=== Test Results ==="
if [ $CLIENT_EXIT_CODE -eq 0 ]; then
    echo "✓ Client completed successfully"
    echo "✓ TensorStore array created and transferred"
    echo "✓ Remote DRAM driver test PASSED"
else
    echo "❌ Client failed with exit code: $CLIENT_EXIT_CODE"
    echo "❌ Remote DRAM driver test FAILED"
fi

echo
echo "Server output should show data verification results above."
echo "Press Ctrl+C to stop the server and exit."

# Keep server running for a bit to see verification output
sleep 5

echo
echo "Test completed. Server will be stopped automatically." 