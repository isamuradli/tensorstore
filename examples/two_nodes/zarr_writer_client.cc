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

/// Simple client that writes tensor data in Zarr format to remote DRAM server

#include <iostream>
#include <vector>

#include "tensorstore/array.h"
#include "tensorstore/data_type.h"
#include "tensorstore/open.h"
#include "tensorstore/spec.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"

int main(int argc, char* argv[]) {
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "=== Zarr Writer Client ===" << std::endl;
  std::cout << "Connecting to remote DRAM server: " << server_addr << std::endl;
  
  // Create a 3D tensor (2x3x4) with test data
  auto tensor = tensorstore::MakeArray<float>(
      {{{1.0f, 2.0f, 3.0f, 4.0f},
        {5.0f, 6.0f, 7.0f, 8.0f},
        {9.0f, 10.0f, 11.0f, 12.0f}},
       {{13.0f, 14.0f, 15.0f, 16.0f},
        {17.0f, 18.0f, 19.0f, 20.0f},
        {21.0f, 22.0f, 23.0f, 24.0f}}});
  
  std::cout << "Created 2x3x4 tensor with shape " << tensor.shape() << std::endl;
  
  // Open TensorStore with Zarr driver using remote_dram kvstore
  auto store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"metadata", {
      {"compressor", nullptr},  // No compression for simplicity
      {"dtype", "<f4"},         // Little-endian 32-bit float
      {"shape", {2, 3, 4}},
      {"chunks", {1, 3, 4}}     // Chunk size
    }}
  }, tensorstore::OpenMode::create).result();
  
  if (!store_result.ok()) {
    std::cerr << "Failed to open TensorStore: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✓ Successfully opened TensorStore with Zarr driver!" << std::endl;
  
  // Write the tensor data to remote DRAM
  std::cout << "\nWriting tensor data in Zarr format..." << std::endl;
  
  auto write_result = tensorstore::Write(tensor, store).result();
  if (!write_result.ok()) {
    std::cerr << "Failed to write tensor: " << write_result.status() << std::endl;
    return 1;
  }
  
  std::cout << "✓ Successfully wrote 2x3x4 tensor to remote DRAM!" << std::endl;
  std::cout << "✓ Data stored in Zarr format with metadata:" << std::endl;
  std::cout << "  - Data type: <f4 (32-bit float)" << std::endl;
  std::cout << "  - Shape: [2, 3, 4]" << std::endl;
  std::cout << "  - Chunk size: [1, 3, 4]" << std::endl;
  std::cout << "  - Compression: none" << std::endl;
  
  // // Write some additional test data to different array positions
  // std::cout << "\nWriting additional test slices..." << std::endl;
  
  // // Create a smaller array for partial writes
  // auto slice_data = tensorstore::MakeArray<float>(
  //     {{100.0f, 200.0f, 300.0f, 400.0f}});
  
  // // Write to specific slice [1, 0, :]
  // auto slice_spec = store | tensorstore::Dims(0, 1).SizedInterval({1, 0}, {1, 1});
  // auto slice_write = tensorstore::Write(slice_data, slice_spec).result();
  
  // if (!slice_write.ok()) {
  //   std::cerr << "Failed to write slice: " << slice_write.status() << std::endl;
  //   return 1;
  // }
  
  // std::cout << "✓ Successfully wrote test slice [1, 0, :] = [100, 200, 300, 400]" << std::endl;
  
  // std::cout << "\n=== Zarr Writer Client Completed Successfully! ===" << std::endl;
  // std::cout << "The tensor data is now stored in the remote DRAM server in Zarr format." << std::endl;
  // std::cout << "You can read it back using a zarr reader client or the same tensorstore spec." << std::endl;
  
  return 0;
}