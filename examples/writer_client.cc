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
  
  std::cout << "🖊️  Writer Client connecting to server: " << server_addr << std::endl;
  
  // Create a TensorStore backed by remote_dram kvstore
  auto store_result = tensorstore::Open({
    {"driver", "zarr"},
    {"kvstore", {
      {"driver", "remote_dram"},
      {"remote_addr", server_addr}
    }},
    {"dtype", "float32"},
    {"metadata", {
      {"shape", {3, 3}},
      {"chunks", {1, 1}}
    }},
    {"create", true},
    {"path", "shared_tensor"}  // Use a shared path for both clients
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "❌ Failed to open tensor store: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✅ Created 3x3 shared tensor store at path 'shared_tensor'" << std::endl;
  
  // Create a distinctive tensor with identifiable values
  auto array = tensorstore::MakeArray<float>(
      {{10.0f, 20.0f, 30.0f}, 
       {40.0f, 50.0f, 60.0f}, 
       {70.0f, 80.0f, 90.0f}});
  
  std::cout << "📝 Writing tensor data..." << std::endl;
  auto write_result = tensorstore::Write(array, store).result();
  if (!write_result.ok()) {
    std::cerr << "❌ Failed to write tensor: " << write_result.status() << std::endl;
    return 1;
  }
  
  std::cout << "✅ Successfully wrote 3x3 tensor (" << (3 * 3 * sizeof(float)) << " bytes)" << std::endl;
  std::cout << "📊 Tensor data written:" << std::endl;
  std::cout << "   [10, 20, 30]" << std::endl;
  std::cout << "   [40, 50, 60]" << std::endl;
  std::cout << "   [70, 80, 90]" << std::endl;
  
  // Sleep for a longer time to allow second client to connect and read
  std::cout << "💤 Writer sleeping for 30 seconds to allow reader client to access the data..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(30));
  
  std::cout << "🏁 Writer client finished" << std::endl;
  return 0;
}