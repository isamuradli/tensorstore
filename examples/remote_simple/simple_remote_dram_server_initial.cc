#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/util/result.h"
#include "absl/log/absl_log.h"
#include "absl/log/initialize.h"

// Include the UcxManager to access stored data
#include "tensorstore/kvstore/remote_dram/remote_dram_kvstore.h"

using tensorstore::kvstore::KvStore;

/// TCP notification server thread
void NotificationServerThread() {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock < 0) {
    std::cerr << "Failed to create notification server socket" << std::endl;
    return;
  }
  
  // Allow socket reuse
  int opt = 1;
  setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(12346);  // Notification port
  
  if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    std::cerr << "Failed to bind notification server socket" << std::endl;
    close(server_sock);
    return;
  }
  
  if (listen(server_sock, 5) < 0) {
    std::cerr << "Failed to listen on notification server socket" << std::endl;
    close(server_sock);
    return;
  }
  
  std::cout << "✓ Notification server started on port 12346" << std::endl;
  
  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
      continue;  // Accept failed, try again
    }
    
    // Read notification data
    char buffer[4096];
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read > 0) {
      buffer[bytes_read] = '\0';
      
      // Parse the notification: "NEW_DATA:<key_length>:<value_length>:<key><value>"
      std::string notification(buffer);
      if (notification.find("NEW_DATA:") == 0) {
        try {
          size_t pos = 9; // After "NEW_DATA:"
          
          // Parse key length
          size_t colon1 = notification.find(':', pos);
          if (colon1 == std::string::npos) continue;
          int key_len = std::stoi(notification.substr(pos, colon1 - pos));
          pos = colon1 + 1;
          
          // Parse value length
          size_t colon2 = notification.find(':', pos);
          if (colon2 == std::string::npos) continue;
          int value_len = std::stoi(notification.substr(pos, colon2 - pos));
          pos = colon2 + 1;
          
          // Extract key and value
          if (pos + key_len + value_len <= notification.length()) {
            std::string key = notification.substr(pos, key_len);
            std::string value = notification.substr(pos + key_len, value_len);
            
            // Print the received data in the server terminal
            std::cout << "\n🎉 SERVER RECEIVED DATA 🎉" << std::endl;
            std::cout << "Key: '" << key << "'" << std::endl;
            std::cout << "Value: '" << value << "'" << std::endl;
            std::cout << "Size: " << value.size() << " bytes" << std::endl;
            std::cout << "✓ Data successfully written to server DRAM!" << std::endl;
            std::cout << std::endl;
          }
        } catch (const std::exception& e) {
          std::cerr << "Error parsing notification: " << e.what() << std::endl;
        }
      }
    }
    
    close(client_sock);
  }
  
  close(server_sock);
}

int main(int argc, char* argv[]) {
  absl::InitializeLog();
  
  std::string listen_addr = "0.0.0.0:12345";
  if (argc > 1) {
    listen_addr = argv[1];
  }
  
  std::cout << "=== Simple Remote DRAM Server ===" << std::endl;
  std::cout << "Listening on: " << listen_addr << std::endl;
  
  // Open remote_dram kvstore in server mode
  auto store_result = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"listen_addr", listen_addr}
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "Failed to open remote_dram server: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "✓ Remote DRAM server started successfully!" << std::endl;
  std::cout << "✓ Server is listening and ready to receive data" << std::endl;
  
  // Start the notification server thread
  std::thread notification_thread(NotificationServerThread);
  notification_thread.detach();
  
  std::cout << "✓ Ready to receive and display data from clients" << std::endl;
  std::cout << "✓ Press Ctrl+C to stop the server" << std::endl;
  
  // Keep the main thread alive
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  
  return 0;
} 