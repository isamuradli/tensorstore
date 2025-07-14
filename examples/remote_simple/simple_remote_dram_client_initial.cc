#include <iostream>
#include <string>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/util/result.h"
#include "absl/log/absl_log.h"
#include "absl/log/initialize.h"
#include "absl/strings/cord.h"

using tensorstore::kvstore::KvStore;

int main(int argc, char* argv[]) {
  absl::InitializeLog();
  
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "=== Simple Remote DRAM Client ===" << std::endl;
  std::cout << "Connecting to server: " << server_addr << std::endl;
  
  // Open remote_dram kvstore in client mode
  auto store_result = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "Failed to connect to remote_dram server: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✓ Connected to remote DRAM server!" << std::endl;
  
  // Write some test data
  std::cout << "\nWriting test data to server..." << std::endl;
  
  // Test data 1
  std::string key1 = "hello";
  std::string value1 = "world from client!";
  auto write_result1 = tensorstore::kvstore::Write(store, key1, absl::Cord(value1)).result();
  if (!write_result1.ok()) {
    std::cerr << "Failed to write data 1: " << write_result1.status() << std::endl;
    return 1;
  }
  std::cout << "✓ Wrote: '" << key1 << "' = '" << value1 << "'" << std::endl;
  
  // Test data 2
  std::string key2 = "test_number";
  std::string value2 = "42";
  auto write_result2 = tensorstore::kvstore::Write(store, key2, absl::Cord(value2)).result();
  if (!write_result2.ok()) {
    std::cerr << "Failed to write data 2: " << write_result2.status() << std::endl;
    return 1;
  }
  std::cout << "✓ Wrote: '" << key2 << "' = '" << value2 << "'" << std::endl;
  
  // Test data 3
  std::string key3 = "message";
  std::string value3 = "UCX communication working!";
  auto write_result3 = tensorstore::kvstore::Write(store, key3, absl::Cord(value3)).result();
  if (!write_result3.ok()) {
    std::cerr << "Failed to write data 3: " << write_result3.status() << std::endl;
    return 1;
  }
  std::cout << "✓ Wrote: '" << key3 << "' = '" << value3 << "'" << std::endl;
  
  std::cout << "\n✓ All data written successfully!" << std::endl;
  std::cout << "✓ Check the server output to see if data was received" << std::endl;
  
  return 0;
} 