#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>
#include <atomic>

#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/util/result.h"
#include "absl/log/absl_log.h"
#include "absl/log/initialize.h"

// Include the UcxManager to access stored data
#include "tensorstore/kvstore/remote_dram/remote_dram_kvstore.h"

using tensorstore::kvstore::KvStore;

std::atomic<bool> server_running{true};

// Forward declaration
void DisplayServerStatus();

void SignalHandler(int signal) {
  std::cout << "\n📡 Received shutdown signal (" << signal << ")" << std::endl;
  server_running = false;
}

/// TCP notification server thread
void NotificationServerThread() {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (server_sock < 0) {
    std::cerr << "❌ Failed to create notification server socket" << std::endl;
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
    std::cerr << "❌ Failed to bind notification server socket" << std::endl;
    close(server_sock);
    return;
  }
  
  if (listen(server_sock, 5) < 0) {
    std::cerr << "❌ Failed to listen on notification server socket" << std::endl;
    close(server_sock);
    return;
  }
  
  std::cout << "📡 Notification server started on port 12346" << std::endl;
  
  while (server_running) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    // Set socket timeout to allow checking server_running periodically
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
      if (!server_running) break;
      continue;  // Accept failed or timeout, try again
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
          if (colon1 == std::string::npos) {
            close(client_sock);
            continue;
          }
          int key_len = std::stoi(notification.substr(pos, colon1 - pos));
          pos = colon1 + 1;
          
          // Parse value length
          size_t colon2 = notification.find(':', pos);
          if (colon2 == std::string::npos) {
            close(client_sock);
            continue;
          }
          int value_len = std::stoi(notification.substr(pos, colon2 - pos));
          pos = colon2 + 1;
          
          // Extract key and value
          if (pos + key_len + value_len <= notification.length()) {
            std::string key = notification.substr(pos, key_len);
            std::string value = notification.substr(pos + key_len, value_len);
            
            // Print the received data in the server terminal
            std::cout << "Data received: '" << key << "' (" << value.size() << " bytes)" << std::endl;
          }
        } catch (const std::exception& e) {
          std::cerr << "❌ Error parsing notification: " << e.what() << std::endl;
        }
      }
    }
    
    close(client_sock);
  }
  
  close(server_sock);
  std::cout << "📡 Notification server stopped" << std::endl;
}

/// Function to display server status and stored data
void DisplayServerStatus() {
  auto& storage = tensorstore::UcxManager::Instance().GetStorage();
  auto key_count = storage.GetKeyCount();
  std::cout << "Server status: " << key_count << " keys stored" << std::endl;
}

/// Status monitoring thread
void StatusMonitorThread() {
  while (server_running) {
    std::this_thread::sleep_for(std::chrono::seconds(30));  // Less frequent updates
    if (server_running) {
      DisplayServerStatus();
    }
  }
}

int main(int argc, char* argv[]) {
  absl::InitializeLog();
  
  // Set up signal handlers for graceful shutdown
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  
  std::string listen_addr = "0.0.0.0:12345";
  if (argc > 1) {
    listen_addr = argv[1];
  }
  
  std::cout << "Remote DRAM Server starting on " << listen_addr << std::endl;
  
  // Open remote_dram kvstore in server mode
  auto store_result = tensorstore::kvstore::Open({
    {"driver", "remote_dram"},
    {"listen_addr", listen_addr}
  }).result();
  
  if (!store_result.ok()) {
    std::cerr << "❌ Failed to open remote_dram server: " << store_result.status() << std::endl;
    return 1;
  }
  
  auto store = *store_result;
  std::cout << "Server started successfully" << std::endl;
  
  // Start the notification server thread
  std::thread notification_thread(NotificationServerThread);
  
  // Start the status monitoring thread
  std::thread status_thread(StatusMonitorThread);
  
  DisplayServerStatus();
  
  // Keep the main thread alive and responsive
  while (server_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  std::cout << "Shutting down server..." << std::endl;
  
  // Wait for background threads to finish
  if (notification_thread.joinable()) {
    notification_thread.join();
  }
  if (status_thread.joinable()) {
    status_thread.join();
  }
  
  std::cout << "Server shutdown complete" << std::endl;
  return 0;
}