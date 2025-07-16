// Simple Writer Client
// Connects to server and writes test data

#include <iostream>
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/context.h"
#include "absl/log/absl_log.h"
#include "absl/strings/cord.h"

int main() {
  std::cout << "Starting Writer Client..." << std::endl;
  
  // Create client context
  auto context = tensorstore::Context::Default();
  
  // Client configuration - connect to server at localhost:12346
  nlohmann::json spec = {
    {"driver", "remote_dram"},
    {"remote_addr", "127.0.0.1:12346"}
  };
  
  try {
    // Open kvstore in client mode
    auto kvstore_result = tensorstore::kvstore::Open(spec, context).result();
    if (!kvstore_result.ok()) {
      std::cerr << "❌ Failed to open kvstore: " << kvstore_result.status() << std::endl;
      return 1;
    }
    
    auto kvstore = kvstore_result.value();
    std::cout << "✅ Client connected to server" << std::endl;
    
    // Create test data
    std::string test_key = "test_key_123";
    std::string test_value = "Hello from writer client!";
    
    std::cout << "Writing data..." << std::endl;
    std::cout << "Key: " << test_key << std::endl;
    std::cout << "Value: " << test_value << std::endl;
    
    // Write data to server
    auto write_result = tensorstore::kvstore::Write(kvstore, test_key, absl::Cord(test_value)).result();
    if (!write_result.ok()) {
      std::cerr << "❌ Failed to write data: " << write_result.status() << std::endl;
      return 1;
    }
    
    std::cout << "✅ Data written successfully to server!" << std::endl;
    std::cout << "Generation: " << write_result.value().generation.value << std::endl;
    
    // Write a second test entry
    std::string test_key2 = "test_key_456";
    std::string test_value2 = "Second test entry";
    
    std::cout << "\nWriting second entry..." << std::endl;
    auto write_result2 = tensorstore::kvstore::Write(kvstore, test_key2, absl::Cord(test_value2)).result();
    if (!write_result2.ok()) {
      std::cerr << "❌ Failed to write second entry: " << write_result2.status() << std::endl;
      return 1;
    }
    
    std::cout << "✅ Second entry written successfully!" << std::endl;
    std::cout << "Writer client completed successfully." << std::endl;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Client error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}