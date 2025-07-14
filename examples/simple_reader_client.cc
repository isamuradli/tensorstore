#include <iostream>
#include <string>
#include <chrono>
#include <thread>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/util/result.h"
#include "absl/log/absl_log.h"
#include "absl/log/initialize.h"

using tensorstore::kvstore::KvStore;

int main(int argc, char* argv[]) {
  absl::InitializeLog();
  
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "📖 Simple Reader Client connecting to server: " << server_addr << std::endl;
  
  // Wait a few seconds to ensure writer has time to connect and write
  std::cout << "⏳ Waiting 8 seconds for writer to initialize and write data..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(8));
  
  // Create a remote_dram kvstore directly
  auto store_result = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "❌ Failed to open kvstore: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✅ Connected to remote DRAM server" << std::endl;
  
  // Read the test data that writer should have written
  std::string key = "test_tensor_data";
  std::string expected_value = "10,20,30,40,50,60,70,80,90";
  
  std::cout << "📖 Reading data from key '" << key << "'..." << std::endl;
  auto read_result = tensorstore::kvstore::Read(store, key).result();
  if (!read_result.ok()) {
    std::cerr << "❌ Failed to read data: " << read_result.status() << std::endl;
    return 1;
  }
  
  if (read_result->state == tensorstore::kvstore::ReadResult::kValue) {
    std::string read_value = std::string(read_result->value);
    std::cout << "✅ Successfully read data from server: " << read_value << std::endl;
    
    // Verify the data matches what writer wrote
    if (read_value == expected_value) {
      std::cout << "✅ Data verification: PASSED - Values match!" << std::endl;
      std::cout << "📊 Tensor data: " << read_value << std::endl;
    } else {
      std::cout << "❌ Data verification: FAILED!" << std::endl;
      std::cout << "   Expected: " << expected_value << std::endl;
      std::cout << "   Got:      " << read_value << std::endl;
      return 1;
    }
  } else if (read_result->state == tensorstore::kvstore::ReadResult::kMissing) {
    std::cout << "❌ Key '" << key << "' not found on server!" << std::endl;
    std::cout << "💡 Make sure the writer client has already written the data!" << std::endl;
    return 1;
  } else {
    std::cout << "❌ Unexpected read result state!" << std::endl;
    return 1;
  }
  
  std::cout << "🏁 Simple reader client finished successfully" << std::endl;
  return 0;
}