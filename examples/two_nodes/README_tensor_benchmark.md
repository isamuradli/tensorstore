# Tensor Benchmark Client

A simple tensor-based benchmarking client for measuring remote_dram performance improvements.

## Purpose

This benchmark client is designed to measure the performance impact of improvements to the remote_dram implementation. It provides:

- Simple tensor data transfer operations
- Clear performance metrics (throughput, latency)
- Minimal benchmarking overhead
- Consistent test workload for comparing before/after performance

## Usage

### 1. Start the Server (Terminal 1)
```bash
# Build and run the server
bazel build //examples/two_nodes:node2_server
./bazel-bin/examples/two_nodes/node2_server 127.0.0.1:12345
```

### 2. Run the Benchmark (Terminal 2)
```bash
# Using the script (recommended)
./examples/two_nodes/run_tensor_benchmark.sh [server_addr] [tensor_size] [iterations]

# Or directly
bazel build //examples/two_nodes:tensor_benchmark_client
./bazel-bin/examples/two_nodes/tensor_benchmark_client [server_addr] [tensor_size] [iterations]
```

### Parameters
- `server_addr`: Server address (default: 127.0.0.1:12345)
- `tensor_size`: Tensor dimension for NxN tensor (default: 64)  
- `iterations`: Number of operations to perform (default: 10)

### Examples
```bash
# Basic benchmark - 64x64 tensors, 10 iterations
./run_tensor_benchmark.sh

# Larger tensors for higher throughput testing
./run_tensor_benchmark.sh 127.0.0.1:12345 128 20

# Smaller tensors for latency testing  
./run_tensor_benchmark.sh 127.0.0.1:12345 32 50
```

## Output Metrics

The benchmark provides:

- **Throughput (MB/s)**: Data transfer rate
- **Average Latency (ms/op)**: Time per operation
- **Total Time**: Overall benchmark duration
- **Data Integrity Check**: Verification of stored data

## Benchmark Design

### Simple and Focused
- No complex benchmark frameworks
- Minimal overhead for accurate measurements
- Clear, interpretable results

### Consistent Workload
- Fixed tensor size and data pattern
- Predictable memory usage
- Repeatable measurements

### Performance Tracking
Ideal for measuring the impact of:
- Buffer management improvements
- UCX optimization changes
- Memory registration enhancements
- Protocol improvements

## Example Output
```
=== Tensor Benchmark Client ===
Server: 127.0.0.1:12345
Tensor size: 64x64
Iterations: 10
✓ Connected to server

=== Tensor Benchmark ===
Tensor size: 64x64 (float32)
Iterations: 10

Bytes per tensor: 16384
Total bytes: 163840

Iteration 1/10 - 15.23ms
Iteration 2/10 - 12.45ms
...

=== Benchmark Results ===
Total Operations: 10
Total Bytes: 163840 (0.16 MB)
Total Time: 145.67 ms
Throughput: 1.10 MB/s
Average Latency: 14.57 ms/op
========================

=== Data integrity check ===
✓ Read verification: 16384 bytes retrieved
First 4 values: 1, 2, 3, 4

Benchmark completed!
```

## Integration with Performance Testing

Use this benchmark to measure improvements at each development phase:

1. **Baseline**: Run before making changes
2. **After each improvement**: Compare with baseline
3. **Track trends**: Monitor performance over multiple changes
4. **Regression testing**: Verify improvements don't break functionality