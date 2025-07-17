#include <iostream>
#include <vector>
#include <string>
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/context.h"
#include "absl/log/absl_log.h"
#include "absl/strings/cord.h"

int main() {
  std::cout << "Starting Multi Writer Client..." << std::endl;
  
  // Create client context
  auto context = tensorstore::Context::Default();
  
  // Client configuration - connect to server at localhost:12346
  nlohmann::json spec = {
    {"driver", "remote_dram"},
    {"remote_addr", "127.0.0.1:12345"}
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
    
    // Define multiple test entries
    std::vector<std::pair<std::string, std::string>> test_data = {
      {"user:alice", "Alice Johnson - Software Engineer"},
      {"user:bob", "Bob Smith - Data Scientist"},
      {"user:charlie", "Charlie Brown - DevOps Engineer"},
      {"config:database_url", "postgresql://localhost:5432/mydb"},
      {"config:cache_size", "1024MB"},
      {"config:max_connections", "100"},
      {"session:sess_abc123", "user_id=alice,expires=2024-12-31"},
      {"session:sess_def456", "user_id=bob,expires=2024-12-31"},
      {"metrics:cpu_usage", "75.5%"},
      {"metrics:memory_usage", "2.1GB"},
      {"document:readme", "This is a sample README file for the project"},
      {"document:changelog", "v1.0.0 - Initial release\nv1.1.0 - Added new features"}
    };
    
    std::cout << "\nWriting " << test_data.size() << " entries to server..." << std::endl;
    
    int success_count = 0;
    int total_count = test_data.size();
    
    // Write each entry
    for (const auto& entry : test_data) {
      const std::string& key = entry.first;
      const std::string& value = entry.second;
      
      std::cout << "Writing: " << key << " -> " << value.substr(0, 50);
      if (value.length() > 50) {
        std::cout << "...";
      }
      std::cout << std::endl;
      
      // Write data to server
      auto write_result = tensorstore::kvstore::Write(kvstore, key, absl::Cord(value)).result();
      if (!write_result.ok()) {
        std::cerr << "❌ Failed to write '" << key << "': " << write_result.status() << std::endl;
      } else {
        std::cout << "✅ Successfully wrote '" << key << "'" << std::endl;
        success_count++;
      }
      
      std::cout << std::endl;
    }
    
    // Summary
    std::cout << "=== Summary ===" << std::endl;
    std::cout << "Total entries: " << total_count << std::endl;
    std::cout << "Successfully written: " << success_count << std::endl;
    std::cout << "Failed: " << (total_count - success_count) << std::endl;
    
    if (success_count == total_count) {
      std::cout << "🎉 All entries written successfully!" << std::endl;
    } else {
      std::cout << "⚠️  Some entries failed to write." << std::endl;
    }
    
    std::cout << "Multi Writer client completed." << std::endl;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Client error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}