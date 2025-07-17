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

/// Node 2 Server: Runs remote_dram server and can read tensor data from its DRAM

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"

int main(int argc, char* argv[]) {
  std::string listen_addr = "0.0.0.0:12345";
  if (argc > 1) {
    listen_addr = argv[1];
  }
  
  std::cout << "=== Node 2 Server ===" << std::endl;
  std::cout << "Starting remote_dram server on: " << listen_addr << std::endl;
  
  // Open remote_dram kvstore in server mode - use JSON directly
  auto kvstore = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"listen_addr", listen_addr}
  }).result();
  
  if (!kvstore.ok()) {
    std::cerr << "Failed to open remote_dram server: " << kvstore.status() << std::endl;
    return 1;
  }
  
  std::cout << "✓ Remote DRAM server started successfully!" << std::endl;
  std::cout << "✓ Server is ready to receive tensor data from Node 1" << std::endl;
  std::cout << "✓ Data will be stored in this node's DRAM memory" << std::endl;
  
  std::cout << "\nServer is running and waiting for connections..." << std::endl;
  std::cout << "All tensor data received from Node 1 will be stored locally" << std::endl;
  std::cout << "Press Ctrl+C to stop the server" << std::endl;
  
  // Keep server running
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "Server still running and ready to receive data..." << std::endl;
  }
  
  return 0;
} 