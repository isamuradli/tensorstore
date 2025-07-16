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

/// Node 1 Writer: Creates a 4x4 tensor and transfers it to Node 2's DRAM

#include <iostream>
#include <vector>

#include "tensorstore/array.h"
#include "tensorstore/data_type.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/open.h"
#include "tensorstore/open_mode.h"
#include "tensorstore/spec.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "absl/strings/cord.h"

int main(int argc, char* argv[]) {
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "=== Node 1 Writer ===" << std::endl;
  std::cout << "Connecting to Node 2 server: " << server_addr << std::endl;
  
  // Create 4x4 tensor with test data
  auto tensor = tensorstore::MakeArray<float>(
      {{1.0f, 2.0f, 3.0f, 4.0f},
       {5.0f, 6.0f, 7.0f, 8.0f}, 
       {9.0f, 10.0f, 11.0f, 12.0f},
       {13.0f, 14.0f, 15.0f, 16.0f}});
  
  std::cout << "Created 4x4 tensor:" << std::endl;
  std::cout << tensor << std::endl;
  
  // Create TensorStore spec with remote_dram backend
  auto spec = tensorstore::Spec::FromJson({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"metadata", {
      {"dtype", "<f4"},  // float32
      {"shape", {4, 4}},
      {"chunks", {2, 2}}
    }}
  });
  
  if (!spec.ok()) {
    std::cerr << "Failed to create TensorStore spec: " << spec.status() << std::endl;
    return 1;
  }
  
  std::cout << "Opening TensorStore with remote_dram backend..." << std::endl;
  
  // Open TensorStore for writing
  auto store = tensorstore::Open(*spec, tensorstore::OpenMode::create).result();
  if (!store.ok()) {
    std::cerr << "Failed to open TensorStore: " << store.status() << std::endl;
    return 1;
  }
  
  std::cout << "Connected to Node 2 successfully!" << std::endl;
  
  // Write tensor to remote DRAM on Node 2
  std::cout << "Writing 4x4 tensor to Node 2's DRAM..." << std::endl;
  auto write_result = tensorstore::Write(tensor, *store).result();
  if (!write_result.ok()) {
    std::cerr << "Failed to write tensor: " << write_result.status() << std::endl;
    return 1;
  }
  
  std::cout << "✓ Successfully transferred 4x4 tensor to Node 2's DRAM!" << std::endl;
  
  // Test direct kvstore operations as well
  std::cout << "Testing direct kvstore write..." << std::endl;
  
  // Use JSON directly for kvstore::Open
  auto kvstore = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }).result();
  
  if (!kvstore.ok()) {
    std::cerr << "Failed to open kvstore: " << kvstore.status() << std::endl;
    return 1;
  }
  
  // Write a simple key-value pair
  std::string test_key = "node1_test";
  absl::Cord test_value("Data from Node 1");
  
  auto kv_write = tensorstore::kvstore::Write(*kvstore, test_key, test_value).result();
  if (!kv_write.ok()) {
    std::cerr << "Failed to write key-value: " << kv_write.status() << std::endl;
    return 1;
  }
  
  std::cout << "✓ Successfully wrote key-value pair to Node 2!" << std::endl;
  std::cout << "  Key: '" << test_key << "'" << std::endl;
  std::cout << "  Value: '" << std::string(test_value) << "'" << std::endl;
  
  std::cout << "\nNode 1 operations completed successfully!" << std::endl;
  return 0;
} 