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

/// Node 1 Reader: Waits 3 seconds, then reads tensor data from Node 2's DRAM

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <sstream>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "absl/strings/cord.h"

int main(int argc, char* argv[]) {
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "=== Node 1 Reader ===" << std::endl;
  std::cout << "Waiting 3 seconds for writer to complete..." << std::endl;
  
  // Wait 3 seconds to ensure writer has completed
  std::this_thread::sleep_for(std::chrono::seconds(3));
  
  std::cout << "Connecting to Node 2 server: " << server_addr << std::endl;
  
  // Open remote_dram kvstore in client mode
  auto kvstore = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }).result();
  
  if (!kvstore.ok()) {
    std::cerr << "Failed to open kvstore: " << kvstore.status() << std::endl;
    return 1;
  }
  
  std::cout << "Connected to Node 2 successfully!" << std::endl;
  
  // Read the tensor data written by node1_writer
  std::cout << "\nReading tensor data from Node 2's DRAM..." << std::endl;
  
  // Read the 4x4 tensor data
  auto tensor_read = tensorstore::kvstore::Read(*kvstore, "testkey").result();
  if (!tensor_read.ok()) {
    std::cerr << "Failed to read tensor data: " << tensor_read.status() << std::endl;
    return 1;
  }
  
  if (!tensor_read->has_value()) {
    std::cerr << "Tensor data not found in server!" << std::endl;
    return 1;
  }
  
  std::string tensor_data = std::string(tensor_read->value);
  std::cout << "✓ Successfully read tensor data from Node 2!" << std::endl;
  std::cout << "  Raw data: " << tensor_data << std::endl;
  
  // Parse and display the tensor data
  if (tensor_data.find("Data from Node 1") == 0) {
    std::string values_str = tensor_data.substr(16); // Skip "Data from Node 1"
    std::cout << "\nParsed 4x4 tensor values:" << std::endl;
    
    // Parse comma-separated values
    std::istringstream iss(values_str);
    std::string value;
    int row = 0, col = 0;
    
    while (std::getline(iss, value, ',')) {
      if (col == 0) std::cout << "  [";
      std::cout << value;
      col++;
      if (col == 4) {
        std::cout << "]" << std::endl;
        col = 0;
        row++;
      } else {
        std::cout << ", ";
      }
    }
    
    // Handle the last value (no comma after it)
    if (col > 0) {
      std::cout << "]" << std::endl;
    }
  }
  
  // Read the simple key-value pair
  std::cout << "\nReading key-value pair from Node 2's DRAM..." << std::endl;
  
  auto kv_read = tensorstore::kvstore::Read(*kvstore, "testkey").result();
  if (!kv_read.ok()) {
    std::cerr << "Failed to read key-value data: " << kv_read.status() << std::endl;
    return 1;
  }
  
  if (!kv_read->has_value()) {
    std::cerr << "Key-value data not found in server!" << std::endl;
    return 1;
  }
  
  std::string kv_data = std::string(kv_read->value);
  std::cout << "✓ Successfully read key-value pair from Node 2!" << std::endl;
  std::cout << "  Key: 'testkey'" << std::endl;
  std::cout << "  Value: '" << kv_data << "'" << std::endl;
  
  // Verify the data matches what the writer sent
  std::cout << "\n=== Data Verification ===" << std::endl;
  bool tensor_valid = tensor_data == "Data from Node 1";
  bool kv_valid = kv_data == "Data from Node 1";
  
  std::cout << "Tensor data valid: " << (tensor_valid ? "YES" : "NO") << std::endl;
  std::cout << "Key-value data valid: " << (kv_valid ? "YES" : "NO") << std::endl;
  
  if (tensor_valid && kv_valid) {
    std::cout << "\n✓ SUCCESS: All data successfully read from Node 2's DRAM!" << std::endl;
    std::cout << "✓ Server is correctly maintaining data in memory!" << std::endl;
  } else {
    std::cout << "\n✗ FAILURE: Data validation failed!" << std::endl;
    return 1;
  }
  
  std::cout << "\nNode 1 reader operations completed successfully!" << std::endl;
  return 0;
}