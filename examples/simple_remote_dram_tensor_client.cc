#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "tensorstore/tensorstore.h"
#include "tensorstore/context.h"
#include "tensorstore/array.h"
#include "tensorstore/data_type.h"
#include "tensorstore/index.h"
#include "tensorstore/open.h"
#include "tensorstore/util/result.h"
#include "tensorstore/index_space/dim_expression.h"
#include "absl/log/absl_log.h"
#include "absl/log/initialize.h"

using tensorstore::Index;
using tensorstore::TensorStore;

int main(int argc, char* argv[]) {
  absl::InitializeLog();
  
  std::string server_addr = "127.0.0.1:12345";
  if (argc > 1) {
    server_addr = argv[1];
  }
  
  std::cout << "Connecting to server: " << server_addr << std::endl;
  
  // Create a TensorStore backed by remote_dram kvstore
  auto store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"dtype", "float32"},
    {"metadata", {
      {"shape", {4, 4}},
      {"chunks", {1, 1}}
    }},
    {"create", true}
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "❌ Failed to open tensor store: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "Created 4x4 tensor store" << std::endl;
  
  auto array = tensorstore::MakeArray<float>(
      {{1.0f, 2.0f, 3.0f, 1.0f}, {6.0f, 7.0f, 8.0f, 2.0f}, {11.0f, 12.0f, 15.0f, 4.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
  
  auto write_result = tensorstore::Write(array, store).result();
  if (!write_result.ok()) {
    std::cerr << "Failed to write tensor: " << write_result.status() << std::endl;
    return 1;
  }
  
  std::cout << "Wrote tensor (" << (4 * 4 * sizeof(float)) << " bytes)" << std::endl;
  
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  
  auto read_result = tensorstore::Read(store).result();
  if (!read_result.ok()) {
    std::cerr << "Failed to read tensor: " << read_result.status() << std::endl;
    return 1;
  }
  
  auto read_array_result = tensorstore::StaticDataTypeCast<float>(*read_result);
  if (!read_array_result.ok()) {
    std::cerr << "Failed to cast read array: " << read_array_result.status() << std::endl;
    return 1;
  }
  auto read_array = *read_array_result;
  std::cout << "Read tensor back from server" << std::endl;
  
  // Verify data integrity
  auto expected_array = tensorstore::MakeArray<float>(
      {{1.0f, 2.0f, 3.0f, 1.0f}, {6.0f, 7.0f, 8.0f, 2.0f}, {11.0f, 12.0f, 15.0f, 4.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
  
  bool data_matches = true;
  for (Index i = 0; i < 4; ++i) {
    for (Index j = 0; j < 4; ++j) {
      if (expected_array(i, j) != read_array(i, j)) {
        data_matches = false;
        break;
      }
    }
  }
  
  std::cout << "Data verification: " << (data_matches ? "PASSED" : "FAILED") << std::endl;
  
  return data_matches ? 0 : 1;
}