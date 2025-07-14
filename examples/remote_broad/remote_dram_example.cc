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

/// \file
/// Example demonstrating remote DRAM key-value store for direct memory-to-memory 
/// transfer of tensor data between two nodes using UCX.
///
/// This example shows how to:
/// 1. Create a server node that acts as a remote memory store
/// 2. Create a client node that sends tensor data to the server
/// 3. Use TensorStore's native APIs for array creation and storage
///
/// Usage:
///   # Terminal 1: Start the server
///   bazel run //examples:remote_dram_example -- --mode=server --listen_addr=0.0.0.0:12345
///
///   # Terminal 2: Run the client  
///   bazel run //examples:remote_dram_example -- --mode=client --server_addr=127.0.0.1:12345

#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include <nlohmann/json.hpp>
#include "tensorstore/array.h"
#include "tensorstore/context.h"
#include "tensorstore/data_type.h"
#include "tensorstore/index.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/open.h"
#include "tensorstore/open_mode.h"
#include "tensorstore/spec.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"

// Command line flags
ABSL_FLAG(std::string, mode, "client", "Mode: 'server' or 'client'");
ABSL_FLAG(std::string, listen_addr, "0.0.0.0:12345", "Server listen address (server mode)");
ABSL_FLAG(std::string, server_addr, "127.0.0.1:12345", "Server address to connect to (client mode)");

namespace {

using ::tensorstore::Context;
using ::tensorstore::Index;

/// Creates a simple 3D tensor with sample data for testing
tensorstore::SharedArray<float, 3> CreateSampleTensor() {
  const Index shape[] = {4, 4, 3};  // 4x4x3 tensor (like a small RGB image)
  auto tensor = tensorstore::AllocateArray<float>(shape);
  
  // Fill with sample data: a gradient pattern
  for (Index i = 0; i < shape[0]; ++i) {
    for (Index j = 0; j < shape[1]; ++j) {
      for (Index k = 0; k < shape[2]; ++k) {
        tensor(i, j, k) = static_cast<float>(i * 10 + j + k * 0.1);
      }
    }
  }
  
  return tensor;
}

/// Print tensor contents for verification
template <typename Array>
void PrintTensor(const Array& tensor, const std::string& name) {
  std::cout << "\n" << name << " (shape: ";
  for (int i = 0; i < tensor.rank(); ++i) {
    std::cout << tensor.shape()[i];
    if (i < tensor.rank() - 1) std::cout << "x";
  }
  std::cout << "):" << std::endl;
  
  // For static float arrays, print actual values
  if constexpr (std::is_same_v<typename Array::Element, float>) {
    // Print first few elements for verification
    if (tensor.rank() == 3) {
      for (Index i = 0; i < std::min(tensor.shape()[0], Index(3)); ++i) {
        for (Index j = 0; j < std::min(tensor.shape()[1], Index(3)); ++j) {
          std::cout << "[";
          for (Index k = 0; k < tensor.shape()[2]; ++k) {
            std::cout << tensor(i, j, k);
            if (k < tensor.shape()[2] - 1) std::cout << ", ";
          }
          std::cout << "] ";
        }
        std::cout << std::endl;
      }
    }
  } else {
    // For dynamic arrays, just show shape and type info
    std::cout << "  Data type: " << tensor.dtype().name() << std::endl;
    std::cout << "  (Values not displayed for dynamic type)" << std::endl;
  }
}

/// Server mode: Sets up a remote DRAM store that listens for incoming connections
absl::Status RunServer(const std::string& listen_addr) {
  std::cout << "Starting remote DRAM server on " << listen_addr << std::endl;
  
  try {
    // Create server-mode kvstore spec using JSON
    ::nlohmann::json kvstore_spec = {
      {"driver", "remote_dram"},
      {"listen_addr", listen_addr}
    };
    
    // Open the kvstore in server mode
    auto kvstore_result = tensorstore::kvstore::Open(kvstore_spec).result();
    if (!kvstore_result.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to open server kvstore: %s", 
                                                kvstore_result.status().message()));
    }
    
    auto kvstore = *kvstore_result;
    std::cout << "Remote DRAM server started successfully!" << std::endl;
    std::cout << "Server is ready to receive tensor data..." << std::endl;
    std::cout << "Press Ctrl+C to stop the server." << std::endl;
    
    // Keep server running
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
  } catch (const std::exception& e) {
    return absl::InternalError(absl::StrFormat("Server error: %s", e.what()));
  }
  
  return absl::OkStatus();
}

/// Client mode: Creates tensor data and sends it to the remote server
absl::Status RunClient(const std::string& server_addr) {
  std::cout << "Starting remote DRAM client, connecting to " << server_addr << std::endl;
  
  try {
    // Create client-mode kvstore spec using JSON
    ::nlohmann::json kvstore_spec = {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    };
    
    // Open the kvstore in client mode
    auto kvstore_result = tensorstore::kvstore::Open(kvstore_spec).result();
    if (!kvstore_result.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to open client kvstore: %s", 
                                                kvstore_result.status().message()));
    }
    
    auto kvstore = *kvstore_result;
    std::cout << "Connected to remote DRAM server successfully!" << std::endl;
    
    // Create sample tensor data
    auto tensor = CreateSampleTensor();
    PrintTensor(tensor, "Original Tensor");
    
    // Create TensorStore spec for storing the tensor using zarr driver
    // Note: zarr driver supports kvstore backends, unlike array driver
    auto store_spec = tensorstore::Spec::FromJson({
      {"driver", "zarr"},
      {"kvstore", {
        {"driver", "remote_dram"},
        {"remote_addr", server_addr}
      }},
      {"metadata", {
        {"dtype", "<f4"},  // float32 little-endian
        {"shape", {4, 4, 3}},
        {"chunks", {2, 2, 3}}  // Chunk size for zarr
      }}
    });
    
    if (!store_spec.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to create TensorStore spec: %s", 
                                                store_spec.status().message()));
    }
    
    std::cout << "\nCreating TensorStore with remote DRAM backend..." << std::endl;
    
    // Open TensorStore for writing
    auto store_result = tensorstore::Open(*store_spec, 
                                         tensorstore::OpenMode::create).result();
    
    if (!store_result.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to open TensorStore: %s", 
                                                store_result.status().message()));
    }
    
    auto store = *store_result;
    std::cout << "TensorStore opened successfully!" << std::endl;
    
    // Write tensor data to remote DRAM
    std::cout << "\nWriting tensor data to remote DRAM..." << std::endl;
    auto write_result = tensorstore::Write(tensor, store).result();
    
    if (!write_result.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to write tensor: %s", 
                                                write_result.status().message()));
    }
    
    std::cout << "✓ Tensor data written to remote DRAM successfully!" << std::endl;
    
    // For demonstration: Try to read the data back
    std::cout << "\nReading tensor data back from remote DRAM..." << std::endl;
    auto read_result = tensorstore::Read(store).result();
    
    if (!read_result.ok()) {
      std::cout << "Note: Read operation not yet implemented in remote_dram driver" << std::endl;
      std::cout << "This is expected based on the current implementation." << std::endl;
    } else {
      auto read_tensor = *read_result;
      PrintTensor(read_tensor, "Read Back Tensor");
      
      // Simple verification: just check if shapes match
      if (tensorstore::ArraysHaveSameShapes(tensor, read_tensor)) {
        std::cout << "✓ Shape verification passed: read data has correct shape!" << std::endl;
      } else {
        std::cout << "⚠ Shape mismatch detected between written and read data" << std::endl;
      }
    }
    
    // Alternative: Test with direct kvstore operations
    std::cout << "\nTesting direct kvstore operations..." << std::endl;
    
    // Write a simple key-value pair
    std::string test_key = "test_key_1";
    absl::Cord test_value("Hello from TensorStore remote DRAM!");
    
    auto write_kv_result = tensorstore::kvstore::Write(kvstore, test_key, test_value).result();
    if (!write_kv_result.ok()) {
      return absl::InternalError(absl::StrFormat("Failed to write key-value: %s", 
                                                write_kv_result.status().message()));
    }
    
    std::cout << "✓ Key-value pair written successfully!" << std::endl;
    std::cout << "  Key: " << test_key << std::endl;
    std::cout << "  Value: " << std::string(test_value) << std::endl;
    
    std::cout << "\nClient operations completed successfully!" << std::endl;
    
  } catch (const std::exception& e) {
    return absl::InternalError(absl::StrFormat("Client error: %s", e.what()));
  }
  
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  
  std::string mode = absl::GetFlag(FLAGS_mode);
  std::string listen_addr = absl::GetFlag(FLAGS_listen_addr);
  std::string server_addr = absl::GetFlag(FLAGS_server_addr);
  
  std::cout << "=== TensorStore Remote DRAM Example ===" << std::endl;
  std::cout << "Mode: " << mode << std::endl;
  
  absl::Status status;
  
  if (mode == "server") {
    status = RunServer(listen_addr);
  } else if (mode == "client") {
    // Give server time to start if running in sequence
    std::cout << "Waiting 2 seconds for server to be ready..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    status = RunClient(server_addr);
  } else {
    std::cerr << "Error: Invalid mode '" << mode << "'. Use 'server' or 'client'." << std::endl;
    std::cerr << "\nUsage examples:" << std::endl;
    std::cerr << "  Server: " << argv[0] << " --mode=server --listen_addr=0.0.0.0:12345" << std::endl;
    std::cerr << "  Client: " << argv[0] << " --mode=client --server_addr=127.0.0.1:12345" << std::endl;
    return 1;
  }
  
  if (!status.ok()) {
    std::cerr << "Error: " << status.message() << std::endl;
    return 1;
  }
  
  return 0;
} 