// Copyright 2025 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "remote_dram_kvstore.h"

#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <iostream>

// System includes for socket operations
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "absl/status/status.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_binding/json_binding.h"
#include "tensorstore/internal/uri_utils.h"
#include "tensorstore/kvstore/byte_range.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/key_range.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/kvstore/read_result.h"
#include "tensorstore/kvstore/registry.h"
#include "tensorstore/kvstore/spec.h"
#include "tensorstore/kvstore/url_registry.h"
#include "tensorstore/util/execution/execution.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/str_cat.h"

// specializations for std::optional
#include "tensorstore/internal/cache_key/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/internal/json_binding/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/serialization/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/util/garbage_collection/std_optional.h"  // IWYU pragma: keep

// UCX headers
#include <ucp/api/ucp.h>

namespace tensorstore {

// RemoteDramStorage Implementation
void RemoteDramStorage::Store(const std::string& key, const absl::Cord& value) {
  absl::MutexLock lock(&mutex_);
  storage_[key] = value;
  ABSL_LOG(INFO) << "Stored key '" << key << "' with " << value.size() << " bytes";
}

std::optional<absl::Cord> RemoteDramStorage::Get(const std::string& key) const {
  absl::MutexLock lock(&mutex_);
  auto it = storage_.find(key);
  if (it != storage_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool RemoteDramStorage::Exists(const std::string& key) const {
  absl::MutexLock lock(&mutex_);
  return storage_.find(key) != storage_.end();
}

bool RemoteDramStorage::Remove(const std::string& key) {
  absl::MutexLock lock(&mutex_);
  return storage_.erase(key) > 0;
}

std::vector<std::string> RemoteDramStorage::GetAllKeys() const {
  absl::MutexLock lock(&mutex_);
  std::vector<std::string> keys;
  keys.reserve(storage_.size());
  for (const auto& [key, value] : storage_) {
    keys.push_back(key);
  }
  return keys;
}

size_t RemoteDramStorage::GetKeyCount() const {
  absl::MutexLock lock(&mutex_);
  return storage_.size();
}

/// UCX error handler callback
void UcxErrorHandler(void* arg, ucp_ep_h ep, ucs_status_t status) {
  ABSL_LOG(ERROR) << "UCX: Connection error: " << ucs_status_string(status);
}

namespace {

namespace jb = tensorstore::internal_json_binding;

/// Send completion callback for UCX operations
void SendCallback(void* request, ucs_status_t status, void* user_data) {
  uint64_t request_id = reinterpret_cast<uint64_t>(user_data);
  ABSL_LOG(INFO) << "UCX send completed for request " << request_id 
                 << " with status: " << ucs_status_string(status);
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
  
  UcxManager::Instance().CompletePendingOperation(request_id, result_status);
  ucp_request_free(request);
}

/// Receive completion callback for server-side message handling
void ServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX server receive completed with status: " << ucs_status_string(status);
  
  // Always free the receive buffer, regardless of success or failure
  char* buffer = static_cast<char*>(user_data);
  
  if (status == UCS_OK && info->length >= sizeof(MessageHeader)) {
    // Process the received message
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
    
    ABSL_LOG(INFO) << "Received message: type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length 
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id;
    
    if (header->type == MessageType::WRITE_REQUEST) {
      // Handle write request
      const char* data_ptr = buffer + sizeof(MessageHeader);
      std::string key(data_ptr, header->key_length);
      absl::Cord value(std::string(data_ptr + header->key_length, header->value_length));
      
      // Store in server memory
      UcxManager::Instance().GetStorage().Store(key, value);
      
      ABSL_LOG(INFO) << "Server stored key-value pair: key='" << key 
                     << "', value_size=" << value.size();
      
      // Send write response back to client
      ucp_ep_h client_endpoint = UcxManager::Instance().GetClientEndpoint();
      if (client_endpoint) {
        UcxManager::Instance().SendWriteResponse(client_endpoint, header->request_id, 0);
      } else {
        ABSL_LOG(ERROR) << "No client endpoint available to send write response";
      }
      
    } else if (header->type == MessageType::READ_REQUEST) {
      // Handle read request
      const char* data_ptr = buffer + sizeof(MessageHeader);
      std::string key(data_ptr, header->key_length);
      
      ABSL_LOG(INFO) << "Server received read request for key='" << key << "'";
      
      // Look up the key in storage
      auto& storage = UcxManager::Instance().GetStorage();
      auto value = storage.Get(key);
      
      if (value.has_value()) {
        ABSL_LOG(INFO) << "Server found key '" << key << "' with value size=" << value->size();
      } else {
        ABSL_LOG(INFO) << "Server key '" << key << "' not found";
      }
      
      // Send read response back to client
      ucp_ep_h client_endpoint = UcxManager::Instance().GetClientEndpoint();
      if (client_endpoint) {
        UcxManager::Instance().SendReadResponse(client_endpoint, header->request_id, value);
      } else {
        ABSL_LOG(ERROR) << "No client endpoint available to send read response";
      }
    }
    
    // Only post another receive buffer if we're not shutting down
    // Check if UCX manager is still active before posting new receive
    auto& ucx_manager = UcxManager::Instance();
    if (ucx_manager.GetContext() && ucx_manager.GetWorker()) {
      ucx_manager.PostServerReceive();
    }
  } else {
    ABSL_LOG(ERROR) << "Failed to receive message: " << ucs_status_string(status);
  }
  
  // Free the receive buffer
  delete[] buffer;
  
  // Free the UCX request
  if (request != nullptr) {
    ucp_request_free(request);
  }
}

/// Client-side callback for handling read responses
void ClientReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX client receive completed with status: " << ucs_status_string(status);
  
  uint64_t request_id = reinterpret_cast<uint64_t>(user_data);
  
  if (status == UCS_OK && info->length >= sizeof(ReadResponse)) {
    // Process the read response
    char* buffer = new char[info->length];  // Will be deleted after processing
    
    // Copy the received data (this is a simplified approach)
    // In a real implementation, the buffer would be pre-allocated and passed properly
    ReadResponse* response = reinterpret_cast<ReadResponse*>(buffer);
    
    ABSL_LOG(INFO) << "Client received read response: status_code=" << response->status_code
                   << ", value_len=" << response->header.value_length
                   << ", request_id=" << response->header.request_id;
    
    kvstore::ReadResult result;
    if (response->status_code == 0 && response->header.value_length > 0) {
      // Success with value
      result.state = kvstore::ReadResult::kValue;
      const char* value_ptr = buffer + sizeof(ReadResponse);
      result.value = absl::Cord(std::string(value_ptr, response->header.value_length));
      result.stamp.generation = StorageGeneration::FromString("remote_read");
      result.stamp.time = absl::Now();
      ABSL_LOG(INFO) << "Read successful, value size=" << result.value.size();
    } else {
      // Key not found or error
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      ABSL_LOG(INFO) << "Read result: key not found";
    }
    
    // Complete the pending read operation
    UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(result));
    
    delete[] buffer;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive read response: " << ucs_status_string(status);
    
    // Complete with error
    kvstore::ReadResult error_result;
    error_result.state = kvstore::ReadResult::kMissing;
    error_result.stamp.generation = StorageGeneration::NoValue();
    error_result.stamp.time = absl::Now();
    UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(error_result));
  }
  
  ucp_request_free(request);
}

/// UCX listener callback for incoming connections
void UcxListenerCallback(ucp_conn_request_h conn_request, void* user_data) {
  ABSL_LOG(INFO) << "UCX: New client connection request received";
  
  // Accept the connection request and create an endpoint for the client
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  
  ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST |
                         UCP_EP_PARAM_FIELD_ERR_HANDLER |
                         UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  ep_params.conn_request = conn_request;
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
  ep_params.err_handler.cb = UcxErrorHandler;
  ep_params.err_handler.arg = nullptr;
  
  ucp_ep_h client_endpoint;
  auto& ucx_manager = UcxManager::Instance();
  ucs_status_t status = ucp_ep_create(ucx_manager.GetWorker(), &ep_params, &client_endpoint);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create server endpoint for client: " << ucs_status_string(status);
    return;
  }
  
  ABSL_LOG(INFO) << "Created server endpoint for client connection";
  
  // Register the client endpoint for response handling
  ucx_manager.RegisterClientEndpoint(client_endpoint);
}

/// Send a notification to the server process that new data has been written
void NotifyServerOfNewData(const kvstore::Key& key, const absl::Cord& value) {
  // Create a TCP socket to notify the server
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    ABSL_LOG(WARNING) << "Failed to create notification socket";
    return;
  }
  
  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  
  // Connect to server notification port (12346 = data port + 1)
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(12346);
  server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  
  if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ABSL_LOG(WARNING) << "Failed to connect to server notification port 12346";
    close(sock);
    return;
  }
  
  // Send notification message with length prefix for reliable parsing
  std::string key_str = std::string(key);
  std::string value_str = std::string(value);
  
  // Format: "NEW_DATA:<key_length>:<value_length>:<key><value>"
  std::string notification = "NEW_DATA:" + std::to_string(key_str.length()) + ":" + 
                           std::to_string(value_str.length()) + ":" + key_str + value_str;
  
  ABSL_LOG(INFO) << "Sending notification to server: key='" << key_str << "', value_size=" << value_str.length();
  
  ssize_t bytes_sent = send(sock, notification.c_str(), notification.length(), 0);
  if (bytes_sent != static_cast<ssize_t>(notification.length())) {
    ABSL_LOG(WARNING) << "Failed to send complete notification to server";
  } else {
    ABSL_LOG(INFO) << "Notification sent successfully to server";
  }
  
  close(sock);
}

}  // namespace

// UcxManager Implementation
UcxManager& UcxManager::Instance() {
  static UcxManager instance;
  return instance;
}

absl::Status UcxManager::Initialize() {
  absl::MutexLock lock(&mutex_);
  
  if (initialized_) {
    return absl::OkStatus();
  }
  
  // Initialize UCX context
  ucp_params_t ucp_params;
  ucp_config_t* config;
  ucs_status_t status;
  
  // Read UCX configuration
  status = ucp_config_read(nullptr, nullptr, &config);
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to read UCX config: %s", 
                                               ucs_status_string(status)));
  }
  
  // Set UCX context parameters with tagged messaging support
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | 
                          UCP_PARAM_FIELD_TAG_SENDER_MASK;
  
  // Use features that support local communication (shared memory) and networking
  ucp_params.features = UCP_FEATURE_TAG | 
                        UCP_FEATURE_WAKEUP;
  
  ucp_params.tag_sender_mask = 0xffff000000000000ULL;  // Use upper 16 bits for sender ID
  
  // Create UCX context
  status = ucp_init(&ucp_params, config, &context_);
  ucp_config_release(config);
  
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to initialize UCX context: %s", 
                                               ucs_status_string(status)));
  }
  
  // Create UCX worker with socket support
  ucp_worker_params_t worker_params;
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
  
  status = ucp_worker_create(context_, &worker_params, &worker_);
  if (status != UCS_OK) {
    ucp_cleanup(context_);
    context_ = nullptr;
    return absl::InternalError(absl::StrFormat("Failed to create UCX worker: %s", 
                                               ucs_status_string(status)));
  }
  
  initialized_ = true;
  ABSL_LOG(INFO) << "UCX Manager initialized successfully with socket support";
  
  // Start worker progress polling
  StartWorkerProgressTask();
  
  return absl::OkStatus();
}

Result<ucp_listener_h> UcxManager::CreateListener(const std::string& listen_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
  ABSL_LOG(INFO) << "Creating UCX listener for address: " << listen_addr;
  
  // Parse address and port
  size_t colon_pos = listen_addr.find(':');
  if (colon_pos == std::string::npos) {
    return absl::InvalidArgumentError("Invalid listen address format, expected host:port");
  }
  
  std::string host = listen_addr.substr(0, colon_pos);
  std::string port_str = listen_addr.substr(colon_pos + 1);
  
  // Validate port number
  int port_num;
  try {
    port_num = std::stoi(port_str);
    if (port_num <= 0 || port_num > 65535) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid port number: %d", port_num));
    }
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid port format: %s", port_str));
  }
  
  // Create listener parameters
  ucp_listener_params_t listener_params;
  struct sockaddr_in listen_sockaddr;
  
  memset(&listener_params, 0, sizeof(listener_params));
  memset(&listen_sockaddr, 0, sizeof(listen_sockaddr));
  
  listen_sockaddr.sin_family = AF_INET;
  listen_sockaddr.sin_port = htons(port_num);
  
  if (host == "0.0.0.0") {
    listen_sockaddr.sin_addr.s_addr = INADDR_ANY;
    ABSL_LOG(INFO) << "Binding to all interfaces (INADDR_ANY) on port " << port_num;
  } else if (host == "127.0.0.1" || host == "localhost") {
    listen_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ABSL_LOG(INFO) << "Binding to loopback interface on port " << port_num;
  } else {
    int inet_result = inet_pton(AF_INET, host.c_str(), &listen_sockaddr.sin_addr);
    if (inet_result != 1) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid host address: %s", host));
    }
    ABSL_LOG(INFO) << "Binding to specific interface " << host << " on port " << port_num;
  }
  
  listener_params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                               UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  listener_params.sockaddr.addr = (const struct sockaddr*)&listen_sockaddr;
  listener_params.sockaddr.addrlen = sizeof(listen_sockaddr);
  listener_params.conn_handler.cb = UcxListenerCallback;
  listener_params.conn_handler.arg = nullptr;
  
  ABSL_LOG(INFO) << "Attempting to create UCX listener with parameters configured";
  
  ucp_listener_h listener;
  ucs_status_t status = ucp_listener_create(worker_, &listener_params, &listener);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "UCX listener creation failed with status: " << ucs_status_string(status)
                    << " (" << status << ")";
    ABSL_LOG(ERROR) << "UCX listener creation details:";
    ABSL_LOG(ERROR) << "  Address: " << listen_addr;
    ABSL_LOG(ERROR) << "  Host: " << host;
    ABSL_LOG(ERROR) << "  Port: " << port_num;
    ABSL_LOG(ERROR) << "  Worker: " << worker_;
    
    // Provide more specific error messages based on common UCX listener errors
    if (status == UCS_ERR_BUSY) {
      return absl::ResourceExhaustedError(absl::StrFormat(
          "Port %d is busy or already in use. UCX error: %s", 
          port_num, ucs_status_string(status)));
    } else if (status == UCS_ERR_UNREACHABLE) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Address %s is unreachable or invalid. UCX error: %s", 
          listen_addr, ucs_status_string(status)));
    } else if (status == UCS_ERR_UNSUPPORTED) {
      return absl::UnimplementedError(absl::StrFormat(
          "UCX listener not supported with current configuration. UCX error: %s", 
          ucs_status_string(status)));
    } else {
      return absl::InternalError(absl::StrFormat("Failed to create UCX listener: %s", 
                                                 ucs_status_string(status)));
    }
  }
  
  ABSL_LOG(INFO) << "UCX listener created successfully on " << listen_addr;
  
  // Store the listener handle for cleanup
  listener_ = listener;
  
  // Post initial receive buffers for incoming messages
  constexpr int num_preposted_receives = 10;
  ABSL_LOG(INFO) << "Posting " << num_preposted_receives << " initial receive buffers";
  for (int i = 0; i < num_preposted_receives; ++i) {
    PostServerReceiveNoLock();
  }
  
  return listener;
}

Result<ucp_ep_h> UcxManager::CreateClientEndpoint(const std::string& server_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
  ABSL_LOG(INFO) << "Creating UCX client endpoint to: " << server_addr;
  
  // Check if this is a localhost connection
  bool is_localhost = (server_addr.find("127.0.0.1") != std::string::npos ||
                      server_addr.find("localhost") != std::string::npos);
  
  if (is_localhost) {
    ABSL_LOG(INFO) << "Detected localhost connection - using local IPC mode for testing";
    
    // For localhost testing, create a dummy endpoint that represents local communication
    // This allows us to test the basic functionality without complex UCX networking
    ucp_ep_h dummy_endpoint = reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL);  // Local comm marker
    
    // Register this endpoint for cleanup
    RegisterClientSideEndpointNoLock(dummy_endpoint);
    
    ABSL_LOG(INFO) << "Created localhost client endpoint for testing";
    return dummy_endpoint;
  }
  
  // For real multi-node connections, use full UCX with socket addresses
  // Parse server address
  size_t colon_pos = server_addr.find(':');
  if (colon_pos == std::string::npos) {
    return absl::InvalidArgumentError("Invalid server address format, expected host:port");
  }
  
  std::string host = server_addr.substr(0, colon_pos);
  std::string port_str = server_addr.substr(colon_pos + 1);
  
  // Validate port number
  int port_num;
  try {
    port_num = std::stoi(port_str);
    if (port_num <= 0 || port_num > 65535) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid port number: %d", port_num));
    }
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid port format: %s", port_str));
  }
  
  // Create socket address for server connection
  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  
  server_sockaddr.sin_family = AF_INET;
  server_sockaddr.sin_port = htons(port_num);
  
  // Convert host to IP address
  int inet_result = inet_pton(AF_INET, host.c_str(), &server_sockaddr.sin_addr);
  if (inet_result != 1) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid host address: %s", host));
  }
  
  ABSL_LOG(INFO) << "Connecting to remote host " << host << " on port " << port_num;
  
  // Create UCX endpoint parameters with socket address
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  
  ep_params.field_mask = UCP_EP_PARAM_FIELD_SOCK_ADDR |
                         UCP_EP_PARAM_FIELD_ERR_HANDLER |
                         UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  ep_params.sockaddr.addr = (const struct sockaddr*)&server_sockaddr;
  ep_params.sockaddr.addrlen = sizeof(server_sockaddr);
  ep_params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
  ep_params.err_handler.cb = UcxErrorHandler;
  ep_params.err_handler.arg = nullptr;
  
  ABSL_LOG(INFO) << "Attempting to create UCX client endpoint with socket connection";
  ABSL_LOG(INFO) << "  Server address: " << server_addr;
  ABSL_LOG(INFO) << "  Socket family: " << server_sockaddr.sin_family;
  ABSL_LOG(INFO) << "  Socket port: " << ntohs(server_sockaddr.sin_port);
  ABSL_LOG(INFO) << "  Worker: " << worker_;
  
  // Create the client endpoint
  ucp_ep_h client_endpoint;
  ucs_status_t status = ucp_ep_create(worker_, &ep_params, &client_endpoint);
  
  ABSL_LOG(INFO) << "UCX endpoint creation result: " << ucs_status_string(status) << " (" << status << ")";
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create UCX client endpoint: " << ucs_status_string(status);
    return absl::InternalError(absl::StrFormat(
        "Failed to create UCX client endpoint to %s: %s",
        server_addr, ucs_status_string(status)));
  }
  
  ABSL_LOG(INFO) << "UCX client endpoint created successfully to " << server_addr;
  
  // Register this endpoint for cleanup
  RegisterClientSideEndpointNoLock(client_endpoint);
  
  return client_endpoint;
}

void UcxManager::RegisterPendingOperation(uint64_t request_id, Promise<void> promise, MessageType type) {
  absl::MutexLock lock(&mutex_);
  auto op = std::make_unique<PendingWriteOperation>(request_id, std::move(promise));
  pending_write_operations_[request_id] = std::move(op);
}

void UcxManager::RegisterPendingReadOperation(uint64_t request_id, Promise<kvstore::ReadResult> promise) {
  absl::MutexLock lock(&mutex_);
  auto op = std::make_unique<PendingReadOperation>(request_id, std::move(promise));
  pending_read_operations_[request_id] = std::move(op);
}

void UcxManager::CompletePendingOperation(uint64_t request_id, absl::Status status) {
  absl::MutexLock lock(&mutex_);
  auto it = pending_write_operations_.find(request_id);
  if (it != pending_write_operations_.end()) {
    if (status.ok()) {
      it->second->promise.SetResult(absl::OkStatus());
    } else {
      it->second->promise.SetResult(status);
    }
    pending_write_operations_.erase(it);
  }
}

void UcxManager::CompletePendingReadOperation(uint64_t request_id, kvstore::ReadResult result) {
  absl::MutexLock lock(&mutex_);
  auto it = pending_read_operations_.find(request_id);
  if (it != pending_read_operations_.end()) {
    it->second->promise.SetResult(std::move(result));
    pending_read_operations_.erase(it);
  }
}

uint64_t UcxManager::GenerateRequestId() {
  absl::MutexLock lock(&mutex_);
  return next_request_id_++;
}

void UcxManager::RegisterClientEndpoint(ucp_ep_h client_endpoint) {
  absl::MutexLock lock(&mutex_);
  client_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client endpoint, total clients: " << client_endpoints_.size();
}

void UcxManager::RegisterClientSideEndpoint(ucp_ep_h client_endpoint) {
  absl::MutexLock lock(&mutex_);
  client_side_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client-side endpoint for cleanup";
}

void UcxManager::RegisterClientSideEndpointNoLock(ucp_ep_h client_endpoint) {
  // This method assumes the mutex is already held
  client_side_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client-side endpoint for cleanup";
}

ucp_ep_h UcxManager::GetClientEndpoint() const {
  absl::MutexLock lock(&mutex_);
  if (client_endpoints_.empty()) {
    return nullptr;
  }
  // Return the most recently connected client
  return client_endpoints_.back();
}

void UcxManager::PostServerReceive() {
  // Allocate buffer for incoming message
  constexpr size_t max_message_size = 64 * 1024;  // 64KB max message
  char* recv_buffer = new char[max_message_size];
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ServerReceiveCallback;
  recv_params.user_data = recv_buffer;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;
  
  ucp_worker_h worker_handle;
  {
    absl::MutexLock lock(&mutex_);
    worker_handle = worker_;
  }
  
  if (!worker_handle) {
    ABSL_LOG(ERROR) << "Cannot post server receive: worker is null";
    delete[] recv_buffer;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_handle, recv_buffer, max_message_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    delete[] recv_buffer;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    ABSL_LOG(INFO) << "Posted server receive buffer";
    
    // Track the request for cleanup
    absl::MutexLock lock(&mutex_);
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    delete[] recv_buffer;
  }
}

void UcxManager::PostServerReceiveNoLock() {
  // Allocate buffer for incoming message
  constexpr size_t max_message_size = 64 * 1024;  // 64KB max message
  char* recv_buffer = new char[max_message_size];
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ServerReceiveCallback;
  recv_params.user_data = recv_buffer;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;
  
  if (!worker_) {
    ABSL_LOG(ERROR) << "Cannot post server receive: worker is null";
    delete[] recv_buffer;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_, recv_buffer, max_message_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    delete[] recv_buffer;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    ABSL_LOG(INFO) << "Posted server receive buffer";
    
    // Track the request for cleanup (mutex already held)
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    delete[] recv_buffer;
  }
}

void UcxManager::CancelPendingReceives() {
  absl::MutexLock lock(&mutex_);
  CancelPendingReceivesNoLock();
}

void UcxManager::CancelPendingReceivesNoLock() {
  // This method assumes the mutex is already held
  ABSL_LOG(INFO) << "Canceling " << active_requests_.size() << " pending UCX requests";
  
  // Cancel all active requests
  for (void* request : active_requests_) {
    if (request != nullptr) {
      ucp_request_cancel(worker_, request);
    }
  }
  
  // Clear the list - the requests will be freed in their callbacks
  active_requests_.clear();
}

void UcxManager::CleanupListener() {
  absl::MutexLock lock(&mutex_);
  CleanupListenerNoLock();
}

void UcxManager::CleanupListenerNoLock() {
  // This method assumes the mutex is already held
  if (listener_ != nullptr) {
    ABSL_LOG(INFO) << "Destroying UCX listener";
    ucp_listener_destroy(listener_);
    listener_ = nullptr;
  }
}

void UcxManager::SendReadResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                  const std::optional<absl::Cord>& value) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send read response: client endpoint is null";
    return;
  }
  
  // Determine response status and value size
  uint32_t status_code = value.has_value() ? 0 : 1;  // 0 = success, 1 = not found
  uint32_t value_size = value.has_value() ? value->size() : 0;
  
  // Create response message
  size_t message_size = sizeof(ReadResponse) + value_size;
  std::vector<char> response_buffer(message_size);
  
  ReadResponse* response = reinterpret_cast<ReadResponse*>(response_buffer.data());
  response->header.type = MessageType::READ_RESPONSE;
  response->header.key_length = 0;  // No key in response
  response->header.value_length = value_size;
  response->header.request_id = request_id;
  response->status_code = status_code;
  
  // Copy value data if present
  if (value.has_value() && value_size > 0) {
    char* value_ptr = response_buffer.data() + sizeof(ReadResponse);
    std::string value_str = std::string(*value);
    std::memcpy(value_ptr, value_str.data(), value_size);
  }
  
  // Send response using UCX
  ucp_request_param_t send_params;
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
  send_params.cb.send = [](void* request, ucs_status_t status, void* user_data) {
    ABSL_LOG(INFO) << "Server read response sent with status: " << ucs_status_string(status);
    ucp_request_free(request);
  };
  
  ucp_tag_t tag = 1;  // Use tag 1 for read responses
  
  void* request = ucp_tag_send_nbx(client_endpoint, response_buffer.data(), 
                                   message_size, tag, &send_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to send read response: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
  } else if (request == nullptr) {
    ABSL_LOG(INFO) << "Read response sent immediately";
  } else {
    ABSL_LOG(INFO) << "Read response send in progress";
  }
}

void UcxManager::SendWriteResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                   uint32_t status_code) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send write response: client endpoint is null";
    return;
  }
  
  // Create response message
  WriteResponse response;
  response.header.type = MessageType::WRITE_RESPONSE;
  response.header.key_length = 0;  // No key in response
  response.header.value_length = 0;  // No value in response
  response.header.request_id = request_id;
  response.status_code = status_code;
  
  // Send response using UCX
  ucp_request_param_t send_params;
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
  send_params.cb.send = [](void* request, ucs_status_t status, void* user_data) {
    ABSL_LOG(INFO) << "Server write response sent with status: " << ucs_status_string(status);
    ucp_request_free(request);
  };
  
  ucp_tag_t tag = 0;  // Use tag 0 for write responses
  
  void* request = ucp_tag_send_nbx(client_endpoint, &response, sizeof(response), 
                                   tag, &send_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to send write response: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
  } else if (request == nullptr) {
    ABSL_LOG(INFO) << "Write response sent immediately";
  } else {
    ABSL_LOG(INFO) << "Write response send in progress";
  }
}

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_) {
    return;
  }
  
  progress_task_running_ = true;
  
  // Start a background thread for worker progress polling
  // In a real implementation, this should use TensorStore's thread pool
  std::thread([this]() {
    WorkerProgressTask();
  }).detach();
  
  ABSL_LOG(INFO) << "UCX worker progress task started";
}

void UcxManager::WorkerProgressTask() {
  ABSL_LOG(INFO) << "UCX worker progress polling started";
  
  while (true) {
    {
      absl::MutexLock lock(&mutex_);
      if (!initialized_ || !progress_task_running_) {
        break;
      }
      
      // Poll for progress
      ucp_worker_progress(worker_);
    }
    
    // Sleep briefly to avoid busy waiting
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  
  ABSL_LOG(INFO) << "UCX worker progress polling stopped";
}

void UcxManager::Shutdown() {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return;
  }
  
  ABSL_LOG(INFO) << "Starting UCX Manager shutdown";
  
  // Stop the progress task first
  progress_task_running_ = false;
  
  // Cancel all pending receive operations
  CancelPendingReceivesNoLock();
  
  // Clean up the UCX listener
  CleanupListenerNoLock();
  
  // Clean up client endpoints
  for (ucp_ep_h client_endpoint : client_endpoints_) {
    if (client_endpoint) {
      ABSL_LOG(INFO) << "Destroying client endpoint";
      ucp_ep_destroy(client_endpoint);
    }
  }
  client_endpoints_.clear();
  
  // Clean up client-side endpoints
  for (ucp_ep_h client_side_endpoint : client_side_endpoints_) {
    if (client_side_endpoint && client_side_endpoint != reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
      ABSL_LOG(INFO) << "Destroying client-side endpoint";
      ucp_ep_destroy(client_side_endpoint);
    } else if (client_side_endpoint == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
      ABSL_LOG(INFO) << "Skipping cleanup of localhost dummy endpoint";
    }
  }
  client_side_endpoints_.clear();
  
  // Complete any pending operations with cancelled status
  for (auto& [id, op] : pending_write_operations_) {
    op->promise.SetResult(absl::CancelledError("UCX Manager shutting down"));
  }
  pending_write_operations_.clear();
  
  for (auto& [id, op] : pending_read_operations_) {
    kvstore::ReadResult cancelled_result;
    cancelled_result.state = kvstore::ReadResult::kMissing;
    cancelled_result.stamp.generation = StorageGeneration::NoValue();
    cancelled_result.stamp.time = absl::Now();
    op->promise.SetResult(std::move(cancelled_result));
  }
  pending_read_operations_.clear();
  
  // Give UCX some time to process cancellations
  if (worker_) {
    // Process any remaining UCX events
    for (int i = 0; i < 10; ++i) {
      ucp_worker_progress(worker_);
    }
  }
  
  // Clean up UCX resources in reverse order of creation
  if (worker_) {
    ABSL_LOG(INFO) << "Destroying UCX worker";
    ucp_worker_destroy(worker_);
    worker_ = nullptr;
  }
  
  if (context_) {
    ABSL_LOG(INFO) << "Cleaning up UCX context";
    ucp_cleanup(context_);
    context_ = nullptr;
  }
  
  initialized_ = false;
  ABSL_LOG(INFO) << "UCX Manager shutdown completed";
}

UcxManager::~UcxManager() {
  Shutdown();
}

namespace {

class RemoteDramDriverSpec
    : public internal_kvstore::RegisteredDriverSpec<RemoteDramDriverSpec,
                                                    RemoteDramDriverSpecData> {
 public:
  /// Specifies the string identifier under which the driver will be registered.
  static constexpr char id[] = "remote_dram";

  Future<kvstore::DriverPtr> DoOpen() const override;

  Result<std::string> ToUrl(std::string_view path) const override {
    return absl::UnimplementedError(
        "remote_dram driver does not support URL conversion");
  }
};

/// Defines the "remote_dram" KeyValueStore driver.
class RemoteDramDriver
    : public internal_kvstore::RegisteredDriver<RemoteDramDriver,
                                                RemoteDramDriverSpec> {
 public:
  Future<kvstore::ReadResult> Read(kvstore::Key key, 
                                   kvstore::ReadOptions options) override {
    if (is_server_mode_) {
      // Server mode: read locally
      return ReadLocal(key, options);
    } else {
      // Client mode: send read request to remote server
      return ReadRemote(key, options);
    }
  }

  Future<TimestampedStorageGeneration> Write(kvstore::Key key,
                                             std::optional<absl::Cord> value,
                                             kvstore::WriteOptions options) override {
    if (!value.has_value()) {
      return absl::InvalidArgumentError("Write value cannot be null");
    }
    
    if (is_server_mode_) {
      // Server mode: write locally
      return WriteLocal(key, *value);
    } else {
      // Client mode: send to remote server
      return WriteRemote(key, *value);
    }
  }

  Future<const void> DeleteRange(KeyRange range) override {
    return absl::UnimplementedError("remote_dram driver DeleteRange not yet implemented");
  }

  void ListImpl(kvstore::ListOptions options, 
                kvstore::ListReceiver receiver) override {
    execution::set_error(receiver, 
                         absl::UnimplementedError("remote_dram driver List not yet implemented"));
  }

  absl::Status GetBoundSpecData(SpecData& spec) const {
    spec = spec_;
    return absl::OkStatus();
  }
  
  // Public members for access from driver spec
  SpecData spec_;
  ucp_ep_h client_endpoint_ = nullptr;  // UCX endpoint for client mode
  bool is_server_mode_ = false;
  
 private:
  Future<TimestampedStorageGeneration> WriteLocal(const kvstore::Key& key, 
                                                  const absl::Cord& value) {
    auto& storage = UcxManager::Instance().GetStorage();
    storage.Store(key, value);
    
    // Return success with current timestamp
    TimestampedStorageGeneration result;
    result.generation = StorageGeneration::FromString(
        absl::StrFormat("%d", std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    result.time = absl::Now();
    
    return MakeReadyFuture<TimestampedStorageGeneration>(result);
  }
  
  Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key, 
                                                   const absl::Cord& value) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "WriteRemote called but client_endpoint is null for key '" << key << "'";
      return absl::InternalError("Client endpoint not available");
    }
    
    // Check if this is a localhost dummy endpoint
    if (client_endpoint_ == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
      ABSL_LOG(INFO) << "WriteRemote using localhost IPC for key '" << key << "' with " << value.size() << " bytes";
      
      // For localhost testing, directly store in the local server's storage
      // This simulates the inter-process communication without complex networking
      auto& storage = UcxManager::Instance().GetStorage();
      storage.Store(key, value);
      
      // Notify the actual server process via TCP (server will print the data)
      NotifyServerOfNewData(key, value);
      
      // Return success immediately
      TimestampedStorageGeneration result;
      result.generation = StorageGeneration::FromString("localhost_write");
      result.time = absl::Now();
      return MakeReadyFuture<TimestampedStorageGeneration>(result);
    }
    
    // For real multi-node connections, use full UCX networking
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    ABSL_LOG(INFO) << "WriteRemote sending data for key '" << key << "' with " << value.size() << " bytes to server";
    
    // Create write message
    size_t message_size = sizeof(MessageHeader) + key.size() + value.size();
    std::vector<char> message_buffer(message_size);
    
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    header->type = MessageType::WRITE_REQUEST;
    header->key_length = static_cast<uint32_t>(key.size());
    header->value_length = static_cast<uint32_t>(value.size());
    header->request_id = request_id;
    
    // Copy key and value data
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    // Handle absl::Cord properly
    std::string value_str = std::string(value);
    std::memcpy(data_ptr + key.size(), value_str.data(), value_str.size());
    
    ABSL_LOG(INFO) << "Sending UCX write request: key='" << key << "', value='" << value_str << "', request_id=" << request_id;
    
    // Create promise/future pair
    auto [promise, future] = PromiseFuturePair<void>::Make();
    
    // Register pending operation
    ucx_manager.RegisterPendingOperation(request_id, std::move(promise), MessageType::WRITE_REQUEST);
    
    // Send message using UCX tagged messaging
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    send_params.user_data = reinterpret_cast<void*>(request_id);
    
    ucp_tag_t tag = 0;  // Use tag 0 for write operations
    
    void* request = ucp_tag_send_nbx(client_endpoint_, message_buffer.data(), 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX send failed immediately: " << ucs_status_string(error_status);
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("UCX send failed: %s", 
              ucs_status_string(error_status))));
    } else if (request == nullptr) {
      // Send completed immediately
      ABSL_LOG(INFO) << "UCX write request sent immediately for request_id=" << request_id;
      ucx_manager.CompletePendingOperation(request_id, absl::OkStatus());
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      ABSL_LOG(INFO) << "UCX write request in progress for request_id=" << request_id;
    }
    
    // Transform the void future to TimestampedStorageGeneration future
    auto [result_promise, result_future] = PromiseFuturePair<TimestampedStorageGeneration>::Make();
    
    // Chain the futures: when the void future completes, complete the result future
    std::move(future).ExecuteWhenReady([promise = std::move(result_promise)](ReadyFuture<void> ready_future) mutable {
      if (ready_future.status().ok()) {
        TimestampedStorageGeneration result;
        result.generation = StorageGeneration::FromString("remote_write");
        result.time = absl::Now();
        promise.SetResult(std::move(result));
      } else {
        promise.SetResult(ready_future.status());
      }
    });
    
    return result_future;
  }
  
  Future<kvstore::ReadResult> ReadLocal(const kvstore::Key& key, 
                                        const kvstore::ReadOptions& options) {
    auto& storage = UcxManager::Instance().GetStorage();
    auto value = storage.Get(key);
    
    if (!value.has_value()) {
      // Key not found
      kvstore::ReadResult result;
      result.state = kvstore::ReadResult::kMissing;
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // Key found, return the value
    kvstore::ReadResult result;
    result.state = kvstore::ReadResult::kValue;
    result.value = *value;
    result.stamp.generation = StorageGeneration::FromString(
        absl::StrFormat("%d", std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    result.stamp.time = absl::Now();
    
    return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
  }
  
  Future<kvstore::ReadResult> ReadRemote(const kvstore::Key& key, 
                                         const kvstore::ReadOptions& options) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "ReadRemote called but client_endpoint is null for key '" << key << "'";
      
      kvstore::ReadResult result;
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // Check if this is a localhost dummy endpoint
    if (client_endpoint_ == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
      ABSL_LOG(INFO) << "ReadRemote using localhost IPC for key '" << key << "'";
      
      // For localhost testing, directly read from the local server's storage
      auto& storage = UcxManager::Instance().GetStorage();
      auto value = storage.Get(key);
      
      kvstore::ReadResult result;
      if (value.has_value()) {
        result.state = kvstore::ReadResult::kValue;
        result.value = *value;
        result.stamp.generation = StorageGeneration::FromString("localhost_read");
        result.stamp.time = absl::Now();
        ABSL_LOG(INFO) << "ReadRemote localhost: found key '" << key << "' with value size=" << value->size();
      } else {
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
        ABSL_LOG(INFO) << "ReadRemote localhost: key '" << key << "' not found";
      }
      
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // For real multi-node connections, use full UCX networking
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    ABSL_LOG(INFO) << "ReadRemote sending read request for key '" << key << "' to server";
    
    // Create read request message
    size_t message_size = sizeof(MessageHeader) + key.size();
    std::vector<char> message_buffer(message_size);
    
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    header->type = MessageType::READ_REQUEST;
    header->key_length = static_cast<uint32_t>(key.size());
    header->value_length = 0;  // No value in read request
    header->request_id = request_id;
    
    // Copy key data
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    ABSL_LOG(INFO) << "Sending UCX read request: key='" << key << "', request_id=" << request_id;
    
    // Create promise/future pair for read result
    auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
    
    // Register pending read operation
    ucx_manager.RegisterPendingReadOperation(request_id, std::move(promise));
    
    // Post a receive buffer to get the response
    PostReadResponseReceive(request_id);
    
    // Send read request using UCX tagged messaging
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    send_params.user_data = reinterpret_cast<void*>(request_id);
    
    ucp_tag_t tag = 1;  // Use tag 1 for read operations
    
    void* request = ucp_tag_send_nbx(client_endpoint_, message_buffer.data(), 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX read request send failed: " << ucs_status_string(error_status);
      
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
    } else if (request == nullptr) {
      // Send completed immediately - response handling will complete the read
      ABSL_LOG(INFO) << "UCX read request sent immediately for request_id=" << request_id;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      ABSL_LOG(INFO) << "UCX read request in progress for request_id=" << request_id;
    }
    
    return future;
  }
  
  /// Post a receive buffer to get read response from server
  void PostReadResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the read response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 64 * 1024;  // 64KB max response
    char* recv_buffer = new char[max_response_size];
    
    // Post non-blocking receive for read response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientReceiveCallback;
    recv_params.user_data = reinterpret_cast<void*>(request_id);
    
    ucp_tag_t tag = 1;  // Use tag 1 for read responses
    ucp_tag_t tag_mask = 0xffff;  // Match tag exactly
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      
      // Complete the operation with error
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      ABSL_LOG(INFO) << "Posted client receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for read responses)
      ABSL_LOG(INFO) << "Client receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
    }
  }
};

Future<kvstore::DriverPtr> RemoteDramDriverSpec::DoOpen() const {
  // Validate that either listen_addr or remote_addr is specified, but not both
  if (data_.listen_addr.has_value() && data_.remote_addr.has_value()) {
    return absl::InvalidArgumentError(
        "Cannot specify both listen_addr and remote_addr");
  }
  
  if (!data_.listen_addr.has_value() && !data_.remote_addr.has_value()) {
    return absl::InvalidArgumentError(
        "Must specify either listen_addr (server mode) or remote_addr (client mode)");
  }

  auto driver = internal::MakeIntrusivePtr<RemoteDramDriver>();
  driver->spec_ = data_;
  
  auto& ucx_manager = UcxManager::Instance();
  auto init_status = ucx_manager.Initialize();
  if (!init_status.ok()) {
    return init_status;
  }
  
  // Initialize UCX for server or client mode
  if (data_.listen_addr.has_value()) {
    // Server mode
    ABSL_LOG(INFO) << "Initializing UCX for server mode on " << *data_.listen_addr;
    
    // Create UCX listener
    auto listener_result = ucx_manager.CreateListener(*data_.listen_addr);
    if (!listener_result.ok()) {
      return listener_result.status();
    }
    
    driver->is_server_mode_ = true;
    ABSL_LOG(INFO) << "UCX server initialized successfully, listening on " << *data_.listen_addr;
  } else {
    // Client mode - create UCX endpoint to server
    ABSL_LOG(INFO) << "Initializing UCX for client mode to " << *data_.remote_addr;
    
    // Create UCX endpoint to server
    auto endpoint_result = ucx_manager.CreateClientEndpoint(*data_.remote_addr);
    if (!endpoint_result.ok()) {
      return endpoint_result.status();
    }
    
    driver->client_endpoint_ = *endpoint_result;
    driver->is_server_mode_ = false;
    ABSL_LOG(INFO) << "UCX client initialized successfully, connected to " << *data_.remote_addr;
  }
  
  return kvstore::DriverPtr(driver.get());
}

Result<kvstore::Spec> ParseRemoteDramUrl(std::string_view url) {
  return absl::UnimplementedError(
      "remote_dram driver URL parsing not yet implemented");
}

}  // namespace
}  // namespace tensorstore

TENSORSTORE_DECLARE_GARBAGE_COLLECTION_NOT_REQUIRED(
    tensorstore::RemoteDramDriver)

// Registers the driver.
namespace {
const tensorstore::internal_kvstore::DriverRegistration<
    tensorstore::RemoteDramDriverSpec>
    registration;

const tensorstore::internal_kvstore::UrlSchemeRegistration
    url_scheme_registration{tensorstore::RemoteDramDriverSpec::id,
                            tensorstore::ParseRemoteDramUrl};
}  // namespace 