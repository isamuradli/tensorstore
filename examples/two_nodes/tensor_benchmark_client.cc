// Copyright 2025 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// Tensor Benchmark Client: Simple tensor data transfer benchmarking for performance testing

#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>

#include "tensorstore/array.h"
#include "tensorstore/data_type.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_format.h"

using namespace std::chrono;

struct BenchmarkResult {
  size_t total_bytes;
  int num_operations;
  double total_time_ms;
  double throughput_mbps;
  double avg_latency_ms;
};

BenchmarkResult RunTensorBenchmark(tensorstore::kvstore::KvStore& kvstore, 
                                   int tensor_size = 64, int num_iterations = 10) {
  std::cout << "\n=== Tensor Benchmark ===\n";
  std::cout << "Tensor size: " << tensor_size << "x" << tensor_size << " (float32)\n";
  std::cout << "Iterations: " << num_iterations << "\n\n";

  // Create tensor data (tensor_size x tensor_size float32)
  auto tensor = tensorstore::AllocateArray<float>({tensor_size, tensor_size});
  
  // Fill with test data
  float* data = tensor.data();
  for (int i = 0; i < tensor_size * tensor_size; ++i) {
    data[i] = static_cast<float>(i + 1);
  }

  // Calculate total bytes per tensor
  size_t bytes_per_tensor = tensor_size * tensor_size * sizeof(float);
  size_t total_bytes = bytes_per_tensor * num_iterations;

  std::cout << "Bytes per tensor: " << bytes_per_tensor << "\n";
  std::cout << "Total bytes: " << total_bytes << "\n\n";

  // Convert tensor to cord for storage
  std::string tensor_data(reinterpret_cast<const char*>(tensor.data()), bytes_per_tensor);
  absl::Cord tensor_cord(tensor_data);

  auto start_time = high_resolution_clock::now();
  
  // Perform write operations
  for (int i = 0; i < num_iterations; ++i) {
    std::string key = absl::StrFormat("tensor_%dx%d_%d", tensor_size, tensor_size, i);
    
    auto write_start = high_resolution_clock::now();
    auto write_result = tensorstore::kvstore::Write(kvstore, key, tensor_cord).result();
    auto write_end = high_resolution_clock::now();
    
    if (!write_result.ok()) {
      std::cerr << "Write failed for iteration " << i << ": " << write_result.status() << std::endl;
      continue;
    }

    auto duration_ms = duration_cast<microseconds>(write_end - write_start).count() / 1000.0;
    std::cout << "Iteration " << (i+1) << "/" << num_iterations 
              << " - " << std::fixed << std::setprecision(2) << duration_ms << "ms\n";
  }

  auto end_time = high_resolution_clock::now();
  auto total_duration = duration_cast<microseconds>(end_time - start_time);
  double total_time_ms = total_duration.count() / 1000.0;

  // Calculate metrics
  double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / (total_time_ms / 1000.0);
  double avg_latency_ms = total_time_ms / num_iterations;

  return {
    .total_bytes = total_bytes,
    .num_operations = num_iterations,
    .total_time_ms = total_time_ms,
    .throughput_mbps = throughput_mbps,
    .avg_latency_ms = avg_latency_ms
  };
}

void PrintResults(const BenchmarkResult& result) {
  std::cout << "\n=== Benchmark Results ===\n";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "Total Operations: " << result.num_operations << "\n";
  std::cout << "Total Bytes: " << result.total_bytes << " (" << (result.total_bytes / (1024.0 * 1024.0)) << " MB)\n";
  std::cout << "Total Time: " << result.total_time_ms << " ms\n";
  std::cout << "Throughput: " << result.throughput_mbps << " MB/s\n";
  std::cout << "Average Latency: " << result.avg_latency_ms << " ms/op\n";
  std::cout << "========================\n\n";
}

int main(int argc, char* argv[]) {
  std::string server_addr = "127.0.0.1:12345";
  int tensor_size = 64;  // Default 64x64 tensor (16KB)
  int num_iterations = 10;
  
  // Parse command line arguments
  if (argc > 1) server_addr = argv[1];
  if (argc > 2) tensor_size = std::atoi(argv[2]);
  if (argc > 3) num_iterations = std::atoi(argv[3]);
  
  std::cout << "=== Tensor Benchmark Client ===\n";
  std::cout << "Server: " << server_addr << "\n";
  std::cout << "Tensor size: " << tensor_size << "x" << tensor_size << "\n";
  std::cout << "Iterations: " << num_iterations << "\n";

  // Connect to remote_dram server
  auto kvstore = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }).result();
  
  if (!kvstore.ok()) {
    std::cerr << "Failed to connect to server: " << kvstore.status() << std::endl;
    return 1;
  }
  
  std::cout << "✓ Connected to server\n";

  // Run benchmark
  auto result = RunTensorBenchmark(*kvstore, tensor_size, num_iterations);
  PrintResults(result);

  // Test a read operation to verify data integrity
  std::cout << "=== Data integrity check ===\n";
  auto read_result = tensorstore::kvstore::Read(*kvstore, "tensor_64x64_0").result();
  if (read_result.ok() && read_result->has_value()) {
    auto data_size = read_result->value.size();
    std::cout << "✓ Read verification: " << data_size << " bytes retrieved\n";
    
    // Verify first few values
    if (data_size >= sizeof(float) * 4) {
      const float* data = reinterpret_cast<const float*>(std::string(read_result->value).data());
      std::cout << "First 4 values: " << data[0] << ", " << data[1] << ", " << data[2] << ", " << data[3] << "\n";
    }
  } else {
    std::cout << "⚠ Read verification failed\n";
  }

  std::cout << "\nBenchmark completed!\n";
  return 0;
}