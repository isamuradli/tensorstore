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
  
  std::cout << "🖊️  Simple Writer Client connecting to server: " << server_addr << std::endl;
  
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
  
  // Write some test data
  std::string key = "test_tensor_data";
  std::string value = "10,20,30,40,50,60,70,80,90";  // 3x3 tensor data
  
  std::cout << "📝 Writing data to key '" << key << "'..." << std::endl;
  auto write_result = tensorstore::kvstore::Write(store, key, absl::Cord(value)).result();
  if (!write_result.ok()) {
    std::cerr << "❌ Failed to write data: " << write_result.status() << std::endl;
    return 1;
  }
  
  std::cout << "✅ Successfully wrote data: " << value << std::endl;
  
  // Verify the write by reading it back
  std::cout << "🔍 Verifying write by reading back..." << std::endl;
  auto read_result = tensorstore::kvstore::Read(store, key).result();
  if (!read_result.ok()) {
    std::cerr << "❌ Failed to read back data: " << read_result.status() << std::endl;
    return 1;
  }
  
  if (read_result->state == tensorstore::kvstore::ReadResult::kValue) {
    std::string read_value = std::string(read_result->value);
    std::cout << "✅ Read back data: " << read_value << std::endl;
    
    if (read_value == value) {
      std::cout << "✅ Data verification: PASSED!" << std::endl;
    } else {
      std::cout << "❌ Data verification: FAILED - Values don't match!" << std::endl;
      return 1;
    }
  } else {
    std::cout << "❌ Data not found after write!" << std::endl;
    return 1;
  }
  
  // Sleep for a while to allow reader client to access the data
  std::cout << "💤 Writer sleeping for 30 seconds to allow reader client to access the data..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(30));
  
  std::cout << "🏁 Simple writer client finished" << std::endl;
  return 0;
}