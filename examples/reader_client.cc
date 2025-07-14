#include <iostream>
#include <string>
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
  
  std::cout << "📖 Reader Client connecting to server: " << server_addr << std::endl;
  
  // Wait a few seconds to ensure writer has time to connect and write
  std::cout << "⏳ Waiting 5 seconds for writer to initialize..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(5));
  
  // Connect to the same TensorStore path that writer used
  auto store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"dtype", "float32"},
    {"open", true},  // Open existing tensor (don't create)
    {"path", "shared_tensor"}  // Same path as writer client
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "❌ Failed to open tensor store: " << store_result.status() << std::endl;
    std::cerr << "💡 Make sure the writer client has already created the tensor!" << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✅ Connected to shared tensor store at path 'shared_tensor'" << std::endl;
  
  // Read the tensor data
  std::cout << "📖 Reading tensor data from remote server..." << std::endl;
  auto read_result = tensorstore::Read(store).result();
  if (!read_result.ok()) {
    std::cerr << "❌ Failed to read tensor: " << read_result.status() << std::endl;
    return 1;
  }
  
  auto read_array_result = tensorstore::StaticDataTypeCast<float>(*read_result);
  if (!read_array_result.ok()) {
    std::cerr << "❌ Failed to cast read array: " << read_array_result.status() << std::endl;
    return 1;
  }
  auto read_array = *read_array_result;
  
  std::cout << "✅ Successfully read tensor from server!" << std::endl;
  
  // Display the read tensor data
  std::cout << "📊 Tensor data read from server:" << std::endl;
  for (Index i = 0; i < 3; ++i) {
    std::cout << "   [";
    for (Index j = 0; j < 3; ++j) {
      std::cout << read_array(i, j);
      if (j < 2) std::cout << ", ";
    }
    std::cout << "]" << std::endl;
  }
  
  // Verify the data matches expected values from writer
  auto expected_array = tensorstore::MakeArray<float>(
      {{10.0f, 20.0f, 30.0f}, 
       {40.0f, 50.0f, 60.0f}, 
       {70.0f, 80.0f, 90.0f}});
  
  bool data_matches = true;
  for (Index i = 0; i < 3; ++i) {
    for (Index j = 0; j < 3; ++j) {
      if (expected_array(i, j) != read_array(i, j)) {
        data_matches = false;
        std::cout << "❌ Mismatch at (" << i << "," << j << "): expected " 
                  << expected_array(i, j) << ", got " << read_array(i, j) << std::endl;
      }
    }
  }
  
  if (data_matches) {
    std::cout << "✅ Data verification: PASSED - All values match writer's data!" << std::endl;
  } else {
    std::cout << "❌ Data verification: FAILED - Some values don't match!" << std::endl;
  }
  
  std::cout << "🏁 Reader client finished" << std::endl;
  return data_matches ? 0 : 1;
}