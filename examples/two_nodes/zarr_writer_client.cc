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
#include <cstdint>

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
  
  // Create a 3D tensor (128x128x64) = 4MB of int32_t data
  // This is 128*128*64 = 1,048,576 integers = 4,194,304 bytes
  const int dim0 = 128;
  const int dim1 = 128; 
  const int dim2 = 64;
  
  std::cout << "Creating large tensor (" << dim0 << "x" << dim1 << "x" << dim2 << ")..." << std::endl;
  
  // Generate test data programmatically
  auto tensor = tensorstore::AllocateArray<int32_t>({dim0, dim1, dim2}, tensorstore::c_order, tensorstore::default_init);
  
  // Fill with test data
  for (int i = 0; i < dim0; ++i) {
    for (int j = 0; j < dim1; ++j) {
      for (int k = 0; k < dim2; ++k) {
        // Create a pattern that's easy to verify
        int32_t value = static_cast<int32_t>(i * 1000000 + j * 1000 + k);
        tensor(i, j, k) = value;
      }
    }
  }
  
  std::cout << "Created " << dim0 << "x" << dim1 << "x" << dim2 << " tensor with shape " << tensor.shape() << std::endl;
  float size_mb = (dim0 * dim1 * dim2 * sizeof(int32_t)) / (1024.0f * 1024.0f);
  std::cout << "Total data size: " << size_mb << " MB" << std::endl;
  
  // Open TensorStore with Zarr driver using remote_dram kvstore
  auto store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"metadata", {
      {"compressor", nullptr},  // No compression for simplicity
      {"dtype", "<i4"},         // Little-endian 32-bit integer
      {"shape", {128, 128, 64}},
      {"chunks", {128, 128, 64}}     // 64x64x64 chunks = 1MB per chunk
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
  
  std::cout << "✓ Successfully wrote " << dim0 << "x" << dim1 << "x" << dim2 << " tensor to remote DRAM!" << std::endl;
  std::cout << "✓ Data stored in Zarr format with metadata:" << std::endl;
  std::cout << "  - Data type: <i4 (32-bit integer)" << std::endl;
  std::cout << "  - Shape: [" << dim0 << ", " << dim1 << ", " << dim2 << "]" << std::endl;
  std::cout << "  - Chunk size: [64, 64, 64]" << std::endl;
  std::cout << "  - Compression: none" << std::endl;
  std::cout << "  - Total size: " << size_mb << " MB" << std::endl;
  
  // // Write some additional test data to different array positions
  // std::cout << "\nWriting additional test slices..." << std::endl;
  
  // // Create a smaller array for partial writes
  // auto slice_data = tensorstore::MakeArray<int32_t>(
  //     {{100, 200, 300, 400}});
  
  // // Write to specific slice [1, 0, :]
  // auto slice_spec = store | tensorstore::Dims(0, 1).SizedInterval({1, 0}, {1, 1});
  // auto slice_write = tensorstore::Write(slice_data, slice_spec).result();
  
  // if (!slice_write.ok()) {
  //   std::cerr << "Failed to write slice: " << slice_write.status() << std::endl;
  //   return 1;
  // }
  
  // std::cout << "✓ Successfully wrote test slice [1, 0, :] = [100, 200, 300, 400]" << std::endl;
  
  // Now read back the data to verify it was stored correctly
  std::cout << "\n=== Reading Back Data for Verification ===" << std::endl;
  
  // Open the same TensorStore for reading
  auto read_store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"metadata", {
      {"dtype", "<i4"},
      {"shape", {128, 128, 64}},
      {"chunks", {128, 128, 64}}
    }}
  }, tensorstore::OpenMode::open).result();
  
  if (!read_store_result.ok()) {
    std::cerr << "Failed to open TensorStore for reading: " << read_store_result.status() << std::endl;
    return 1;
  }
  
  auto read_store = *read_store_result;
  std::cout << "✓ Successfully opened TensorStore for reading!" << std::endl;
  
  // Read the entire tensor back
  auto read_result = tensorstore::Read(read_store).result();
  if (!read_result.ok()) {
    std::cerr << "Failed to read tensor: " << read_result.status() << std::endl;
    return 1;
  }
  
  auto read_tensor = *read_result;
  std::cout << "✓ Successfully read tensor back from remote DRAM!" << std::endl;
  std::cout << "Read tensor shape: " << read_tensor.shape() << std::endl;
  
  // Verify the data matches what we wrote
  std::cout << "\nData verification:" << std::endl;
  std::cout << "Original tensor shape: " << tensor.shape() << std::endl;
  std::cout << "Read tensor shape: " << read_tensor.shape() << std::endl;
  
  // Check if shapes match first
  bool shapes_match = (tensor.shape().size() == read_tensor.shape().size());
  if (shapes_match) {
    for (int i = 0; i < tensor.shape().size(); ++i) {
      if (tensor.shape()[i] != read_tensor.shape()[i]) {
        shapes_match = false;
        break;
      }
    }
  }
  
  bool data_matches = false;
  
  if (!shapes_match) {
    std::cout << "❌ Shape mismatch!" << std::endl;
    std::cout << "\nOriginal tensor data:" << std::endl;
    std::cout << tensor << std::endl;
    std::cout << "\nRead tensor data:" << std::endl;
    std::cout << read_tensor << std::endl;
    data_matches = false;
  } else {
    // Compare element by element using raw data access
    std::cout << "✓ Shapes match, comparing data..." << std::endl;
    
    // Get raw data pointers
    const int32_t* original_data = tensor.data();
    const void* read_data = read_tensor.data();
    
    data_matches = true;
    size_t num_elements = dim0 * dim1 * dim2;  // 128x128x64 tensor
    
    std::cout << "\nVerifying " << num_elements << " elements..." << std::endl;
    
    // For large data, sample verification instead of printing all
    size_t mismatches = 0;
    
    for (size_t i = 0; i < num_elements; ++i) {
      int32_t original_val = original_data[i];
      int32_t read_val = static_cast<const int32_t*>(read_data)[i];
      
      if (original_val != read_val) {
        mismatches++;
        if (mismatches <= 10) {  // Only print first 10 mismatches
          std::cout << "  Mismatch at element[" << i << "] original=" << original_val 
                    << ", read=" << read_val << std::endl;
        }
        data_matches = false;
      }
    }
    
    if (mismatches > 0) {
      std::cout << "✗ Found " << mismatches << " mismatches out of " << num_elements << " elements" << std::endl;
    } else {
      std::cout << "✓ All " << num_elements << " elements verified successfully!" << std::endl;
    }
    
    if (data_matches) {
      std::cout << "✓ All " << num_elements << " elements match!" << std::endl;
    }
  }
  if (data_matches) {
    std::cout << "\n✅ SUCCESS: All data matches! Zarr format write/read completed successfully!" << std::endl;
    std::cout << "✅ Remote DRAM server correctly stored and retrieved tensor data in Zarr format!" << std::endl;
  } else {
    std::cout << "\n❌ FAILURE: Data mismatch detected!" << std::endl;
    return 1;
  }
  
  std::cout << "\n=== Zarr Writer/Reader Client Completed Successfully! ===" << std::endl;
  std::cout << "The tensor data was successfully written to and read from the remote DRAM server in Zarr format." << std::endl;
  
  return 0;
}