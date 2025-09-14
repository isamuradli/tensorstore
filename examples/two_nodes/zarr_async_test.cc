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

/// Simple async test to see how remote_dram handles concurrency internally via UCX

#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

#include "tensorstore/array.h"
#include "tensorstore/data_type.h"
#include "tensorstore/open.h"
#include "tensorstore/spec.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/future.h"

int main(int argc, char* argv[]) {
  std::string server_addr = "127.0.0.1:12345";
  int num_operations = 10;
  
  if (argc > 1) server_addr = argv[1];
  if (argc > 2) num_operations = std::stoi(argv[2]);
  
  std::cout << "=== Remote DRAM Internal Concurrency Test ===" << std::endl;
  std::cout << "Server: " << server_addr << std::endl;
  std::cout << "Concurrent operations: " << num_operations << std::endl;
  std::cout << "Let remote_dram + UCX handle concurrency internally..." << std::endl;
  std::cout << std::endl;
  
  auto start_time = std::chrono::steady_clock::now();
  
  // Create multiple tensors with different data
  std::vector<tensorstore::SharedArray<int32_t>> tensors;
  std::vector<tensorstore::Future<tensorstore::TensorStore<>>> stores;
  
  for (int i = 0; i < num_operations; ++i) {
    // Create tensor with unique data pattern
    const int dim0 = 64;
    const int dim1 = 64; 
    const int dim2 = 32;
    
    auto tensor = tensorstore::AllocateArray<int32_t>({dim0, dim1, dim2}, tensorstore::c_order, tensorstore::default_init);
    
    // Fill with operation-specific pattern
    for (int x = 0; x < dim0; ++x) {
      for (int y = 0; y < dim1; ++y) {
        for (int z = 0; z < dim2; ++z) {
          int32_t value = static_cast<int32_t>(i * 1000000 + x * 1000 + y * 10 + z);
          tensor(x, y, z) = value;
        }
      }
    }
    
    tensors.push_back(tensor);
    
    // Create unique zarr path for this operation
    std::string zarr_path = "/async_op_" + std::to_string(i);
    
    // Start async open - this will be handled by remote_dram internally
    auto store_future = tensorstore::Open({
      {"driver", "zarr"},
      {"kvstore", {
        {"driver", "remote_dram"},
        {"remote_addr", server_addr},
        {"path", zarr_path}
      }},
      {"metadata", {
        {"compressor", nullptr},
        {"dtype", "<i4"},
        {"shape", {dim0, dim1, dim2}},
        {"chunks", {dim0, dim1, dim2}}
      }}
    }, tensorstore::OpenMode::create);
    
    stores.push_back(store_future);
  }
  
  std::cout << "Started " << num_operations << " async store opens..." << std::endl;
  
  // Start all write operations concurrently
  std::vector<tensorstore::Future<tensorstore::WriteFutures>> write_futures;
  
  for (int i = 0; i < num_operations; ++i) {
    auto store_result = stores[i].result();
    if (!store_result.ok()) {
      std::cerr << "Failed to open store " << i << ": " << store_result.status() << std::endl;
      continue;
    }
    
    auto store = *store_result;
    std::cout << "Store " << i << " opened, starting write..." << std::endl;
    
    // Start async write - remote_dram will handle this concurrently
    auto write_future = tensorstore::Write(tensors[i], store);
    write_futures.push_back(write_future);
  }
  
  std::cout << "All " << write_futures.size() << " writes started, waiting for completion..." << std::endl;
  
  // Wait for all writes to complete
  int successful_writes = 0;
  for (size_t i = 0; i < write_futures.size(); ++i) {
    auto write_result = write_futures[i].result();
    if (write_result.ok()) {
      successful_writes++;
      std::cout << "Write " << i << " completed ✓" << std::endl;
    } else {
      std::cerr << "Write " << i << " failed: " << write_result.status() << std::endl;
    }
  }
  
  std::cout << std::endl << "All writes completed. Starting concurrent reads..." << std::endl;
  
  // Wait for all reads and verify
  int successful_operations = 0;
  
  for (int i = 0; i < successful_writes; ++i) {
    auto store_result = stores[i].result();
    if (!store_result.ok()) continue;
    
    auto store = *store_result;
    std::cout << "Starting read " << i << "..." << std::endl;
    
    // Start async read - remote_dram will handle this concurrently
    auto read_result = tensorstore::Read(store).result();
    if (!read_result.ok()) {
      std::cerr << "Read " << i << " failed: " << read_result.status() << std::endl;
      continue;
    }
    
    auto read_tensor = *read_result;
    
    // Quick verification
    const int32_t* original_data = tensors[i].data();
    const int32_t* read_data = static_cast<const int32_t*>(read_tensor.data());
    
    bool matches = true;
    size_t num_elements = 64 * 64 * 32;
    
    for (size_t j = 0; j < num_elements; ++j) {
      if (original_data[j] != read_data[j]) {
        matches = false;
        break;
      }
    }
    
    if (matches) {
      successful_operations++;
      std::cout << "Operation " << i << " verified ✓" << std::endl;
    } else {
      std::cerr << "Operation " << i << " data mismatch ❌" << std::endl;
    }
  }
  
  auto end_time = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  
  // Results
  std::cout << std::endl;
  std::cout << "=== Results ===" << std::endl;
  std::cout << "Successful operations: " << successful_operations << "/" << num_operations << std::endl;
  std::cout << "Total time: " << duration.count() << " ms" << std::endl;
  std::cout << "Average time per operation: " << (duration.count() / (float)num_operations) << " ms" << std::endl;
  
  float data_size_mb = (64 * 64 * 32 * sizeof(int32_t) * successful_operations) / (1024.0f * 1024.0f);
  std::cout << "Total data processed: " << data_size_mb << " MB" << std::endl;
  std::cout << "Throughput: " << (data_size_mb * 1000.0f / duration.count()) << " MB/s" << std::endl;
  
  if (successful_operations == num_operations) {
    std::cout << std::endl << "✅ SUCCESS: remote_dram handled all concurrent operations!" << std::endl;
    std::cout << "UCX and remote_dram internal concurrency working correctly!" << std::endl;
    return 0;
  } else {
    std::cout << std::endl << "❌ Some operations failed" << std::endl;
    return 1;
  }
}