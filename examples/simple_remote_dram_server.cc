// Simple Remote DRAM Server
// Starts a server that listens for client connections and stores data

#include <iostream>
#include <thread>
#include <chrono>
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/context.h"
#include "absl/log/absl_log.h"

int main() {
  std::cout << "Starting Remote DRAM Server..." << std::endl;
  
  // Create server context
  auto context = tensorstore::Context::Default();
  
  // Server configuration - listen on localhost:12346
  nlohmann::json spec = {
    {"driver", "remote_dram"},
    {"listen_addr", "127.0.0.1:12346"}
  };
  
  try {
    // Open kvstore in server mode
    auto kvstore_result = tensorstore::kvstore::Open(spec, context).result();
    if (!kvstore_result.ok()) {
      std::cerr << "❌ Failed to open kvstore: " << kvstore_result.status() << std::endl;
      return 1;
    }
    
    std::cout << "✅ Server started successfully on 127.0.0.1:12346" << std::endl;
    std::cout << "Server is running. Press Ctrl+C to stop." << std::endl;
    
    // Keep server running
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      
      // Print server status every 5 seconds
      std::cout << "Server status: Running, listening for connections..." << std::endl;
    }
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Server error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}