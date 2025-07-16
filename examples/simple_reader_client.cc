// Simple Reader Client
// Connects to server and reads test data

#include <iostream>
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/context.h"
#include "absl/log/absl_log.h"
#include "absl/strings/cord.h"

int main() {
  std::cout << "Starting Reader Client..." << std::endl;
  
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
    
    // Try to read the first test entry
    std::string test_key = "test_key_123";
    
    std::cout << "Reading data..." << std::endl;
    std::cout << "Key: " << test_key << std::endl;
    
    auto read_result = tensorstore::kvstore::Read(kvstore, test_key, {}).result();
    if (!read_result.ok()) {
      std::cerr << "❌ Failed to read data: " << read_result.status() << std::endl;
      return 1;
    }
    
    auto result = read_result.value();
    
    if (result.state == tensorstore::kvstore::ReadResult::kMissing) {
      std::cout << "❌ Key not found on server" << std::endl;
    } else if (result.state == tensorstore::kvstore::ReadResult::kValue) {
      std::string value = std::string(result.value);
      std::cout << "✅ Data read successfully!" << std::endl;
      std::cout << "Value: " << value << std::endl;
      std::cout << "Generation: " << result.stamp.generation.value << std::endl;
    }
    
    // Try to read the second test entry
    std::string test_key2 = "test_key_456";
    
    std::cout << "\nReading second entry..." << std::endl;
    std::cout << "Key: " << test_key2 << std::endl;
    
    auto read_result2 = tensorstore::kvstore::Read(kvstore, test_key2, {}).result();
    if (!read_result2.ok()) {
      std::cerr << "❌ Failed to read second entry: " << read_result2.status() << std::endl;
      return 1;
    }
    
    auto result2 = read_result2.value();
    
    if (result2.state == tensorstore::kvstore::ReadResult::kMissing) {
      std::cout << "❌ Second key not found on server" << std::endl;
    } else if (result2.state == tensorstore::kvstore::ReadResult::kValue) {
      std::string value2 = std::string(result2.value);
      std::cout << "✅ Second entry read successfully!" << std::endl;
      std::cout << "Value: " << value2 << std::endl;
    }
    
    // Test reading non-existent key
    std::cout << "\nTesting non-existent key..." << std::endl;
    auto read_result3 = tensorstore::kvstore::Read(kvstore, "non_existent_key", {}).result();
    if (!read_result3.ok()) {
      std::cerr << "❌ Failed to read non-existent key: " << read_result3.status() << std::endl;
      return 1;
    }
    
    auto result3 = read_result3.value();
    if (result3.state == tensorstore::kvstore::ReadResult::kMissing) {
      std::cout << "✅ Non-existent key correctly reported as missing" << std::endl;
    }
    
    std::cout << "\nReader client completed successfully." << std::endl;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Client error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}