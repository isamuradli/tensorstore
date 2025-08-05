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
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <memory>
#include <queue>
#include <condition_variable>
#include <functional>

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
#include "tensorstore/util/executor.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "absl/time/time.h"
#include "absl/time/clock.h"

// specializations for std::optional
#include "tensorstore/internal/cache_key/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/internal/json_binding/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/serialization/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/util/garbage_collection/std_optional.h"  // IWYU pragma: keep

// UCX headers
#include <ucp/api/ucp.h>

namespace tensorstore {

// Enhanced callback dispatcher to safely schedule work from UCX callbacks
// This replaces the dangerous detached thread pattern
class UcxCallbackDispatcher {
 private:
  static std::queue<std::function<void()>> callback_queue_;
  static std::mutex queue_mutex_;
  static std::condition_variable queue_cv_;
  static std::atomic<bool> worker_running_;
  static std::thread worker_thread_;
  
  // Static cleanup helper to ensure thread is joined before program exit
  struct CleanupHelper {
    ~CleanupHelper() {
      if (worker_running_.load()) {
        UcxCallbackDispatcher::Shutdown();
      }
    }
  };
  static CleanupHelper cleanup_helper_;
  
  static void WorkerLoop() {
    while (worker_running_.load() || !callback_queue_.empty()) {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [] { 
        return !callback_queue_.empty() || !worker_running_.load(); 
      });
      
      if (!callback_queue_.empty()) {
        auto callback = std::move(callback_queue_.front());
        callback_queue_.pop();
        lock.unlock();
        
        // Execute callback outside of mutex to avoid deadlock
        callback();
      }
    }
  }
  
 public:
  static void Initialize() {
    if (!worker_running_.exchange(true)) {
      worker_thread_ = std::thread(WorkerLoop);
    }
  }
  
  static void Shutdown() {
    worker_running_.store(false);
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }
  
  template<typename Fn>
  static void ScheduleCallback(Fn&& fn) {
    // Initialize worker if not already running
    Initialize();
    
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      callback_queue_.emplace(std::forward<Fn>(fn));
    }
    queue_cv_.notify_one();
  }
};

// Static member definitions for UcxCallbackDispatcher
std::queue<std::function<void()>> UcxCallbackDispatcher::callback_queue_;
std::mutex UcxCallbackDispatcher::queue_mutex_;
std::condition_variable UcxCallbackDispatcher::queue_cv_;
std::atomic<bool> UcxCallbackDispatcher::worker_running_{false};
std::thread UcxCallbackDispatcher::worker_thread_;
UcxCallbackDispatcher::CleanupHelper UcxCallbackDispatcher::cleanup_helper_;


// Enhanced UCX Buffer wrapper with proper RAII for memory safety
class UcxBuffer {
 public:
  static std::unique_ptr<UcxBuffer> Create(size_t size) {
    return std::make_unique<UcxBuffer>(size);
  }
  
  explicit UcxBuffer(size_t size) : size_(size), data_(new char[size]) {}
  ~UcxBuffer() { delete[] data_; }
  
  // Non-copyable, movable
  UcxBuffer(const UcxBuffer&) = delete;
  UcxBuffer& operator=(const UcxBuffer&) = delete;
  UcxBuffer(UcxBuffer&&) = default;
  UcxBuffer& operator=(UcxBuffer&&) = default;
  
  char* data() { return data_; }
  const char* data() const { return data_; }
  size_t size() const { return size_; }
  
 private:
  size_t size_;
  char* data_;
};

// Server-side task for handling read responses safely with reference counting
struct ServerReadResponseTask : public internal::AtomicReferenceCount<ServerReadResponseTask> {
  ucp_ep_h client_endpoint;
  uint64_t request_id;
  std::optional<absl::Cord> value;
  std::unique_ptr<UcxBuffer> send_buffer;
  
  ServerReadResponseTask(ucp_ep_h ep, uint64_t id, std::optional<absl::Cord> val)
    : client_endpoint(ep), request_id(id), value(std::move(val)) {}
  
  void Start() {
    // Create response message
    uint32_t status_code = value.has_value() ? 0 : 1;
    uint32_t value_size = value.has_value() ? value->size() : 0;
    size_t total_size = sizeof(ReadResponse) + value_size;
    
    send_buffer = UcxBuffer::Create(total_size);
    
    ReadResponse* response = reinterpret_cast<ReadResponse*>(send_buffer->data());
    response->header.magic_number = MESSAGE_MAGIC_NUMBER;
    response->header.type = MessageType::READ_RESPONSE;
    response->header.key_length = 0;
    response->header.value_length = value_size;
    response->header.request_id = request_id;
    response->status_code = status_code;
    
    if (value.has_value() && value_size > 0) {
      std::string value_str = std::string(*value);
      response->header.checksum = message_utils::CalculateChecksum(
          value_str.data(), value_size);
      std::memcpy(send_buffer->data() + sizeof(ReadResponse), 
                  value_str.data(), value_size);
    } else {
      response->header.checksum = 0;
    }
    
    // Send response with proper reference counting
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = ServerSendCallback;
    send_params.user_data = this;
    intrusive_ptr_increment(this);  // For callback
    
    void* request = ucp_tag_send_nbx(client_endpoint, send_buffer->data(),
                                     total_size, UCX_TAG_READ_RESPONSE, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to send read response: " 
                      << ucs_status_string(UCS_PTR_STATUS(request));
      intrusive_ptr_decrement(this);  // Cleanup
    }
  }
  
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* task = static_cast<ServerReadResponseTask*>(user_data);
    ABSL_LOG(INFO) << "Server read response sent: " << ucs_status_string(status);
    
    intrusive_ptr_decrement(task);  // Release reference
    ucp_request_free(request);
  }
};

// Server-side task for handling write responses safely with reference counting
struct ServerWriteResponseTask : public internal::AtomicReferenceCount<ServerWriteResponseTask> {
  ucp_ep_h client_endpoint;
  uint64_t request_id;
  uint32_t status_code;
  std::unique_ptr<UcxBuffer> send_buffer;
  
  ServerWriteResponseTask(ucp_ep_h ep, uint64_t id, uint32_t status)
    : client_endpoint(ep), request_id(id), status_code(status) {}
  
  void Start() {
    send_buffer = UcxBuffer::Create(sizeof(WriteResponse));
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(send_buffer->data());
    response->header.magic_number = MESSAGE_MAGIC_NUMBER;
    response->header.type = MessageType::WRITE_RESPONSE;
    response->header.key_length = 0;
    response->header.value_length = 0;
    response->header.request_id = request_id;
    response->header.checksum = 0;
    response->status_code = status_code;
    
    // Send response with proper reference counting
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = ServerSendCallback;
    send_params.user_data = this;
    intrusive_ptr_increment(this);  // For callback
    
    void* request = ucp_tag_send_nbx(client_endpoint, send_buffer->data(),
                                     sizeof(WriteResponse), UCX_TAG_WRITE_RESPONSE, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to send write response: " 
                      << ucs_status_string(UCS_PTR_STATUS(request));
      intrusive_ptr_decrement(this);  // Cleanup
    }
  }
  
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* task = static_cast<ServerWriteResponseTask*>(user_data);
    ABSL_LOG(INFO) << "Server write response sent: " << ucs_status_string(status);
    
    intrusive_ptr_decrement(task);  // Release reference
    ucp_request_free(request);
  }
};

// RemoteDramStorage Implementation
void RemoteDramStorage::Store(const std::string& key, const absl::Cord& value) {
  absl::MutexLock lock(&mutex_);
  storage_[key] = value;
  // ABSL_LOG(INFO) << "Stored key '" << key << "' with value: " << absl::StrCat(value) << " ";
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
  // Handle the new user_data structure with UCX pool tracking
  struct SendUserData {
    uint64_t request_id;
    char* buffer_to_free;
    bool from_ucx_pool;  // Track if buffer came from UCX pool or regular allocation
  };
  
  SendUserData* data = static_cast<SendUserData*>(user_data);
  uint64_t request_id = data->request_id;
  char* buffer_to_free = data->buffer_to_free;
  bool from_ucx_pool = data->from_ucx_pool;
  
  // ABSL_LOG(INFO) << "UCX send completed for request " << request_id 
  //                << " with status: " << ucs_status_string(status);
  
  if (status == UCS_OK) {
    // std::cout << "✅ UCX: Send completed successfully [ID:" << request_id << "]" << std::endl;
  } else {
    std::cout << "❌ UCX: Send failed [ID:" << request_id << "] - " << ucs_status_string(status) << std::endl;
  }
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
  
  // PHASE 1 OPTIMIZATION 4: Direct processing for simple operations
  // This is a simple operation that can be processed directly in UCX context
  UcxManager::Instance().CompletePendingOperationDirect(request_id, result_status);
  
  // PHASE 1 OPTIMIZATION 3: Proper buffer cleanup based on source
  if (from_ucx_pool) {
    // Return buffer to UCX pool for reuse
    UcxManager::Instance().GetBufferPool().ReleaseBuffer(buffer_to_free);
  } else {
    // Regular allocated buffer, use delete[]
    delete[] buffer_to_free;
  }
  delete data;
  
  ucp_request_free(request);
}

/// Connection callback for server-side client connections
void UcxListenerCallback(ucp_conn_request_h conn_request, void* user_data) {
  ABSL_LOG(INFO) << "UCX listener received connection request";
  
  // Accept the connection request
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  ep_params.conn_request = conn_request;
  
  ucp_ep_h server_endpoint;
  ucs_status_t status = ucp_ep_create(UcxManager::Instance().GetWorker(), &ep_params, &server_endpoint);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create server endpoint: " << ucs_status_string(status);
    return;
  }
  
  ABSL_LOG(INFO) << "Server endpoint created successfully for incoming connection";
  
  // ENHANCED: Register this endpoint as a client endpoint for responses
  // Use safer callback dispatcher to avoid deadlock
  UcxCallbackDispatcher::ScheduleCallback([server_endpoint]() {
    UcxManager::Instance().RegisterClientEndpoint(server_endpoint);
  });
}

/// Adaptive receive completion callback for server-side message handling
void AdaptiveServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX adaptive server receive completed with status: " << ucs_status_string(status);
  
  // Extract adaptive user data structure
  struct AdaptiveReceiveUserData {
    char* buffer;
    bool is_from_pool;
    size_t buffer_size;
  };
  
  AdaptiveReceiveUserData* adaptive_data = static_cast<AdaptiveReceiveUserData*>(user_data);
  char* buffer = adaptive_data->buffer;
  bool is_from_pool = adaptive_data->is_from_pool;
  // size_t buffer_size = adaptive_data->buffer_size;  // Currently unused
  
  if (status == UCS_OK && info->length >= sizeof(MessageHeader)) {
    // Log message for debugging
    message_utils::LogMessageBuffer(buffer, info->length, "Server received adaptive message");
    
    // Process the received message
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
    
    // Verify message integrity
    if (!message_utils::VerifyMessageHeader(header, info->length)) {
      ABSL_LOG(ERROR) << "Adaptive message integrity verification failed, discarding message";
      goto cleanup_and_exit;
    }
    
    ABSL_LOG(INFO) << "Adaptive received message: magic=" << std::dec << header->magic_number
                   << ", type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length 
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id
                   << ", checksum=" << header->checksum;
    
    if (header->type == MessageType::WRITE_REQUEST) {
      // Handle write request
      const char* data_ptr = buffer + sizeof(MessageHeader);
      
      // Verify payload checksum
      if (header->checksum != 0) {
        uint32_t calculated_checksum = message_utils::CalculateChecksum(data_ptr, 
                                                                       header->key_length + header->value_length);
        if (calculated_checksum != header->checksum) {
          ABSL_LOG(ERROR) << "Adaptive payload checksum mismatch";
          goto cleanup_and_exit;
        }
      }
      
      std::string key(data_ptr, header->key_length);
      absl::Cord value(std::string(data_ptr + header->key_length, header->value_length));
      
      // Store in server memory
      UcxManager::Instance().GetStorage().Store(key, value);
      
      ABSL_LOG(INFO) << "Adaptive server stored key-value pair: key='" << key 
                     << "', value_size=" << value.size();
      
      // Send write response back to client using safe callback dispatcher
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      UcxCallbackDispatcher::ScheduleCallback([request_id, key_copy]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendWriteResponse(client_endpoint, request_id, 0);  // 0 = success
          ABSL_LOG(INFO) << "Sent adaptive write response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send adaptive write response";
        }
      });
      
    } else if (header->type == MessageType::READ_REQUEST) {
      // Handle read request
      const char* data_ptr = buffer + sizeof(MessageHeader);
      std::string key(data_ptr, header->key_length);
      
      ABSL_LOG(INFO) << "Adaptive server received read request for key='" << key << "'";
      
      // Look up the key in storage
      auto& storage = UcxManager::Instance().GetStorage();
      auto value = storage.Get(key);
      
      if (value.has_value()) {
        ABSL_LOG(INFO) << "Adaptive server found key '" << key << "' with value size=" << value->size();
      } else {
        ABSL_LOG(INFO) << "Adaptive server key '" << key << "' not found";
      }
      
      // Send read response back to client using safe callback dispatcher
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      UcxCallbackDispatcher::ScheduleCallback([request_id, key_copy, value]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendReadResponse(client_endpoint, request_id, value);
          ABSL_LOG(INFO) << "Sent adaptive read response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send adaptive read response";
        }
      });
    }
    
    // Post another adaptive receive buffer if we're not shutting down
    // Use a safer approach without capturing reference
    UcxCallbackDispatcher::ScheduleCallback([]() {
      auto& ucx_manager = UcxManager::Instance();
      if (ucx_manager.GetContext() && ucx_manager.GetWorker()) {
        ucx_manager.PostAdaptiveServerReceive();
      }
    });
  } else {
    ABSL_LOG(ERROR) << "Failed to receive adaptive message: " << ucs_status_string(status);
  }
  
cleanup_and_exit:
  // Clean up the buffer - return to pool if it came from pool
  if (is_from_pool) {
    UcxManager::Instance().GetBufferPool().ReleaseBuffer(buffer);
  } else {
    delete[] buffer;
  }
  
  // Clean up user data
  delete adaptive_data;
  
  // Free the UCX request
  if (request != nullptr) {
    ucp_request_free(request);
  }
}

/// Receive completion callback for server-side message handling
void ServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX server receive completed with status: " << ucs_status_string(status);
  
  // Always free the receive buffer, regardless of success or failure
  char* buffer = static_cast<char*>(user_data);
  
  if (status == UCS_OK && info->length >= sizeof(MessageHeader)) {
    // Log message for debugging
    message_utils::LogMessageBuffer(buffer, info->length, "Server received message");
    
    // Process the received message
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
    
    // Verify message integrity
    if (!message_utils::VerifyMessageHeader(header, info->length)) {
      ABSL_LOG(ERROR) << "Message integrity verification failed, discarding message";
      delete[] buffer;
      if (request != nullptr) {
        ucp_request_free(request);
      }
      return;
    }
    
    size_t header_offset = 0;  // No offset needed with proper buffer management
    
    ABSL_LOG(INFO) << "Received message: magic=" << std::dec << header->magic_number
                   << ", type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length 
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id
                   << ", checksum=" << header->checksum;
    
    // Try to extract readable content from the message payload
    // Skip the header and look for the actual key-value data
    if (info->length > sizeof(MessageHeader)) {
      const char* payload = buffer + sizeof(MessageHeader);
      size_t payload_size = info->length - sizeof(MessageHeader);
      
      // Look for the key-value pattern: should be "node1_test" followed by "Data from Node 1"
      std::string payload_str(payload, payload_size);
      // ABSL_LOG(INFO) << "Raw payload content: '" << payload_str << "'";
      
      // Try to find readable strings in the payload
      std::string readable_payload;
      for (size_t i = 0; i < payload_size; ++i) {
        char c = payload[i];
        if (c >= 32 && c <= 126) {  // Printable ASCII
          readable_payload += c;
        } else if (c == 0) {
          readable_payload += " [NULL] ";
        } else {
          readable_payload += " [" + std::to_string(static_cast<unsigned char>(c)) + "] ";
        }
      }
      // ABSL_LOG(INFO) << "Readable payload: " << readable_payload;
      
      // Try to find the expected key and value strings in the buffer
      std::string expected_key = "node1_test";
      std::string expected_value = "Data from Node 1";
      
      // Search for the key string
      std::string entire_buffer(buffer, info->length);
      size_t key_pos = entire_buffer.find(expected_key);
      size_t value_pos = entire_buffer.find(expected_value);
      
      if (key_pos != std::string::npos) {
        ABSL_LOG(INFO) << "Found expected key '" << expected_key << "' at position " << key_pos;
      }
      if (value_pos != std::string::npos) {
        ABSL_LOG(INFO) << "Found expected value '" << expected_value << "' at position " << value_pos;
      }
      
      if (key_pos != std::string::npos && value_pos != std::string::npos) {
        ABSL_LOG(INFO) << "SUCCESS: Both key and value found in received buffer!";
        ABSL_LOG(INFO) << "  Key: '" << expected_key << "' at position " << key_pos;
        ABSL_LOG(INFO) << "  Value: '" << expected_value << "' at position " << value_pos;
      }
    }
    
    if (header->type == MessageType::WRITE_REQUEST) {
      // Handle write request
      const char* data_ptr = buffer + header_offset + sizeof(MessageHeader);
      
      // Verify payload checksum
      if (header->checksum != 0) {
        uint32_t calculated_checksum = message_utils::CalculateChecksum(data_ptr, 
                                                                       header->key_length + header->value_length);
        if (calculated_checksum != header->checksum) {
          ABSL_LOG(ERROR) << "Payload checksum mismatch: expected " << std::dec << header->checksum
                          << ", calculated " << std::dec << calculated_checksum;
          delete[] buffer;
          if (request != nullptr) {
            ucp_request_free(request);
          }
          return;
        }
      }
      
      std::string key(data_ptr, header->key_length);
      absl::Cord value(std::string(data_ptr + header->key_length, header->value_length));
      
      // Store in server memory
      UcxManager::Instance().GetStorage().Store(key, value);
      
      ABSL_LOG(INFO) << "Server stored key-value pair: key='" << key 
                     << "', value_size=" << value.size();
      
      std::cout << "📥 SERVER: Received write request [ID:" << header->request_id << "] key='" << key 
                << "' value='" << std::string(value) << "' - STORED SUCCESS" << std::endl;
      
      // ENHANCED: Send write response back to client using safe callback dispatcher
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      UcxCallbackDispatcher::ScheduleCallback([request_id, key_copy]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendWriteResponse(client_endpoint, request_id, 0);  // 0 = success
          ABSL_LOG(INFO) << "Sent write response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send write response for key: " << key_copy;
        }
      });
      
    } else if (header->type == MessageType::READ_REQUEST) {
      // Handle read request
      const char* data_ptr = buffer + header_offset + sizeof(MessageHeader);
      std::string key(data_ptr, header->key_length);
      
      ABSL_LOG(INFO) << "Server received read request for key='" << key << "'";
      
      // std::cout << "📬 SERVER: Received read request [ID:" << header->request_id << "] key='" << key << "'" << std::endl;
      
      // Look up the key in storage
      auto& storage = UcxManager::Instance().GetStorage();
      auto value = storage.Get(key);
      
      if (value.has_value()) {
        ABSL_LOG(INFO) << "Server found key '" << key << "' with value size=" << value->size();
        // std::cout << "📤 SERVER: Found key '" << key << "' [ID:" << header->request_id << "] - sending response with " << value->size() << " bytes" << std::endl;
      } else {
        ABSL_LOG(INFO) << "Server key '" << key << "' not found";
        std::cout << "❌ SERVER: Key '" << key << "' not found [ID:" << header->request_id << "]" << std::endl;
      }
      
      // ENHANCED: Send read response back to client using safe callback dispatcher
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      UcxCallbackDispatcher::ScheduleCallback([request_id, key_copy, value]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendReadResponse(client_endpoint, request_id, value);
          ABSL_LOG(INFO) << "Sent read response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send read response for key: " << key_copy;
        }
      });
    }
    
    // Only post another receive buffer if we're not shutting down
    // Check if UCX manager is still active before posting new receive
    auto& ucx_manager = UcxManager::Instance();
    if (ucx_manager.GetContext() && ucx_manager.GetWorker()) {
      // ENHANCED: Use safer callback dispatcher to avoid deadlock
      UcxCallbackDispatcher::ScheduleCallback([&ucx_manager]() {
        ucx_manager.PostServerReceive();
      });
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
  // ABSL_LOG(INFO) << "UCX client receive completed with status: " << ucs_status_string(status);
  
  // Extract user data structure
  struct ReadUserData {
    uint64_t request_id;
    char* buffer;
  };
  ReadUserData* read_data = static_cast<ReadUserData*>(user_data);
  uint64_t request_id = read_data->request_id;
  char* buffer = read_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(ReadResponse)) {
    // The data was received into the buffer we provided
    // Copy to ensure we have valid data access
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content and hex dump
    // ABSL_LOG(INFO) << "Client received " << info->length << " bytes";
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received read response");
    
    // Add hex dump for debugging the exact buffer layout
    std::stringstream hex_dump;
    for (size_t i = 0; i < std::min(info->length, static_cast<size_t>(64)); ++i) {
      hex_dump << std::hex << std::setfill('0') << std::setw(2) 
               << static_cast<unsigned char>(response_data[i]) << " ";
      if ((i + 1) % 16 == 0) hex_dump << "\n";
    }
    // ABSL_LOG(INFO) << "Hex dump of received data:\n" << hex_dump.str();
    
    ReadResponse* response = reinterpret_cast<ReadResponse*>(response_data.data());
    
    // ABSL_LOG(INFO) << "Client received read response: status_code=" << response->status_code
    //              << ", value_len=" << response->header.value_length
    //              << ", request_id=" << response->header.request_id;
    
    kvstore::ReadResult result;
    if (response->status_code == 0 && response->header.value_length > 0) {
      // Validate value_length is reasonable
      if (response->header.value_length > 1000000) {  // 1MB max sanity check
        ABSL_LOG(ERROR) << "Received invalid value_length: " << response->header.value_length;
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
      } else if (info->length < sizeof(ReadResponse) + response->header.value_length) {
        ABSL_LOG(ERROR) << "Received message too small for claimed value_length";
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
      } else {
        // Success with value
        result.state = kvstore::ReadResult::kValue;
        const char* value_ptr = response_data.data() + sizeof(ReadResponse);
        result.value = absl::Cord(std::string(value_ptr, response->header.value_length));
        result.stamp.generation = StorageGeneration::FromString("remote_read");
        result.stamp.time = absl::Now();
        // ABSL_LOG(INFO) << "Read successful, value size=" << result.value.size();
        // std::cout << "📥 CLIENT: Read response [ID:" << response->header.request_id << "] SUCCESS - received " << result.value.size() << " bytes" << std::endl;
      }
    } else {
      // Key not found or error
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      ABSL_LOG(INFO) << "Read result: key not found";
      std::cout << "❌ CLIENT: Read response [ID:" << response->header.request_id << "] KEY NOT FOUND" << std::endl;
    }
    
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple read response
    UcxManager::Instance().CompletePendingReadOperationDirect(request_id, std::move(result));
    
    delete[] buffer;
    delete read_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive read response: " << ucs_status_string(status);
    
    // ENHANCED: Complete with error using safe callback dispatcher
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple read error  
    kvstore::ReadResult error_result;
    error_result.state = kvstore::ReadResult::kMissing;
    error_result.stamp.generation = StorageGeneration::NoValue();
    error_result.stamp.time = absl::Now();
    UcxManager::Instance().CompletePendingReadOperationDirect(request_id, std::move(error_result));
    
    delete[] buffer;
    delete read_data;
  }
  
  ucp_request_free(request);
}

/// Client-side callback for handling write responses
void ClientWriteResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  // ABSL_LOG(INFO) << "UCX client write response received with status: " << ucs_status_string(status);
  
  // Extract user data structure (same pattern as read)
  struct WriteUserData {
    uint64_t request_id;
    char* buffer;
  };
  WriteUserData* write_data = static_cast<WriteUserData*>(user_data);
  uint64_t request_id = write_data->request_id;
  char* buffer = write_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(WriteResponse)) {
    // The data was received into the buffer we provided
    // Copy to ensure we have valid data access (same pattern as read)
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content
    // ABSL_LOG(INFO) << "Client received write response " << info->length << " bytes";
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received write response");
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(response_data.data());
    
    // ABSL_LOG(INFO) << "Client received write response: status_code=" << response->status_code
    //                << ", request_id=" << response->header.request_id;
    
    absl::Status result_status;
    if (response->status_code == 0) {
      // Success
      result_status = absl::OkStatus();
      // ABSL_LOG(INFO) << "Write successful for request_id=" << response->header.request_id;
      // std::cout << "📥 CLIENT: Write response [ID:" << response->header.request_id << "] SUCCESS" << std::endl;
    } else {
      // Error
      result_status = absl::InternalError("Write failed on server");
      ABSL_LOG(INFO) << "Write failed for request_id=" << response->header.request_id;
      std::cout << "❌ CLIENT: Write response [ID:" << response->header.request_id << "] FAILED" << std::endl;
    }
    
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple write response
    UcxManager::Instance().CompletePendingOperationDirect(request_id, result_status);
    
    delete[] buffer;
    delete write_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive write response: " << ucs_status_string(status);
    
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple error case
    UcxManager::Instance().CompletePendingOperationDirect(request_id, 
        absl::InternalError(absl::StrFormat("Failed to receive write response: %s", 
                                           ucs_status_string(status))));
    
    delete[] buffer;
    delete write_data;
  }
  
  ucp_request_free(request);
}



}  // namespace

// PHASE 1 OPTIMIZATION 1 + 3: Zero-Copy Buffer Management with UCX Integration
class ZeroCopyMessageBuilder {
public:
  static std::unique_ptr<char[]> BuildWriteMessage(
      const kvstore::Key& key, 
      const absl::Cord& value,
      uint64_t request_id,
      size_t* total_size) {
    
    *total_size = sizeof(MessageHeader) + key.size() + value.size();
    auto buffer = std::make_unique<char[]>(*total_size);
    
    // Initialize header directly in buffer
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.get());
    char* payload = buffer.get() + sizeof(MessageHeader);
    
    // Direct key copy (no intermediate storage)
    std::memcpy(payload, key.data(), key.size());
    
    // Direct cord-to-buffer copy (avoid string conversion)
    size_t offset = key.size();
    for (absl::string_view chunk : value.Chunks()) {
      std::memcpy(payload + offset, chunk.data(), chunk.size());
      offset += chunk.size();
    }
    
    // Initialize header with pre-calculated checksum
    message_utils::InitializeMessageHeader(
        header, MessageType::WRITE_REQUEST,
        static_cast<uint32_t>(key.size()),
        static_cast<uint32_t>(value.size()),
        request_id, payload);
    
    return buffer;
  }

  // PHASE 1 OPTIMIZATION 3: UCX-aware message construction with registered memory
  static char* BuildWriteMessageUcx(
      const kvstore::Key& key,
      const absl::Cord& value,
      uint64_t request_id,
      size_t* total_size,
      ucp_mem_h* mem_handle) {
    
    *total_size = sizeof(MessageHeader) + key.size() + value.size();
    
    // Try to get UCX-registered buffer from pool
    auto& buffer_pool = UcxManager::Instance().GetBufferPool();
    size_t actual_size;
    char* buffer = buffer_pool.AcquireUcxBuffer(*total_size, &actual_size, mem_handle);
    
    if (buffer) {
      // Initialize header directly in registered buffer
      MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
      char* payload = buffer + sizeof(MessageHeader);
      
      // Direct key copy (no intermediate storage)
      std::memcpy(payload, key.data(), key.size());
      
      // Direct cord-to-buffer copy (avoid string conversion)
      size_t offset = key.size();
      for (absl::string_view chunk : value.Chunks()) {
        std::memcpy(payload + offset, chunk.data(), chunk.size());
        offset += chunk.size();
      }
      
      // Initialize header with pre-calculated checksum
      message_utils::InitializeMessageHeader(
          header, MessageType::WRITE_REQUEST,
          static_cast<uint32_t>(key.size()),
          static_cast<uint32_t>(value.size()),
          request_id, payload);
      
      return buffer;
    }
    
    // Fallback to regular allocation if UCX buffer not available
    auto fallback_buffer = BuildWriteMessage(key, value, request_id, total_size);
    *mem_handle = nullptr;
    return fallback_buffer.release();
  }

  static std::unique_ptr<char[]> BuildReadMessage(
      const kvstore::Key& key,
      uint64_t request_id,
      size_t* total_size) {
    
    *total_size = sizeof(MessageHeader) + key.size();
    auto buffer = std::make_unique<char[]>(*total_size);
    
    // Initialize header directly in buffer
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.get());
    char* payload = buffer.get() + sizeof(MessageHeader);
    
    // Direct key copy (no intermediate storage)
    std::memcpy(payload, key.data(), key.size());
    
    // Initialize header with pre-calculated checksum
    message_utils::InitializeMessageHeader(
        header, MessageType::READ_REQUEST,
        static_cast<uint32_t>(key.size()),
        0, // No value for read request
        request_id, payload);
    
    return buffer;
  }
};

// Message utility functions implementation
namespace message_utils {

uint32_t CalculateChecksum(const void* data, size_t size) {
  uint32_t checksum = 0;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    checksum = (checksum << 1) ^ bytes[i];
  }
  return checksum;
}

bool VerifyMessageHeader(const MessageHeader* header, size_t total_message_size) {
  if (!header) {
    ABSL_LOG(ERROR) << "Message header is null";
    return false;
  }
  
  // Check magic number
  if (header->magic_number != MESSAGE_MAGIC_NUMBER) {
    ABSL_LOG(ERROR) << "Invalid magic number: expected " << std::dec << MESSAGE_MAGIC_NUMBER
                    << ", got " << std::dec << header->magic_number;
    return false;
  }
  
  // Check message type
  if (header->type != MessageType::WRITE_REQUEST && 
      header->type != MessageType::WRITE_RESPONSE &&
      header->type != MessageType::READ_REQUEST &&
      header->type != MessageType::READ_RESPONSE) {
    ABSL_LOG(ERROR) << "Invalid message type: " << static_cast<uint32_t>(header->type);
    return false;
  }
  
  // Check total message size consistency
  size_t expected_size = sizeof(MessageHeader) + header->key_length + header->value_length;
  if (total_message_size < expected_size) {
    ABSL_LOG(ERROR) << "Message size mismatch: expected at least " << expected_size
                    << " bytes, got " << total_message_size;
    return false;
  }
  
  return true;
}

void InitializeMessageHeader(MessageHeader* header, MessageType type, 
                            uint32_t key_length, uint32_t value_length, 
                            uint64_t request_id, const void* payload_data) {
  header->magic_number = MESSAGE_MAGIC_NUMBER;
  header->type = type;
  header->key_length = key_length;
  header->value_length = value_length;
  header->request_id = request_id;
  
  // Calculate checksum for payload data
  if (payload_data && (key_length + value_length) > 0) {
    header->checksum = CalculateChecksum(payload_data, key_length + value_length);
  } else {
    header->checksum = 0;
  }
}

void LogMessageBuffer(const char* buffer, size_t size, const std::string& prefix) {
  // ABSL_LOG(INFO) << prefix << " (" << size << " bytes)";
  
  // Log the message header interpretation if present
  if (size >= sizeof(MessageHeader)) {
    const MessageHeader* header = reinterpret_cast<const MessageHeader*>(buffer);
    // ABSL_LOG(INFO) << "  Header: magic=" << std::dec << header->magic_number
    //                << ", type=" << static_cast<uint32_t>(header->type)
    //                << ", key_len=" << header->key_length
    //                << ", value_len=" << header->value_length
    //                << ", request_id=" << header->request_id
    //                << ", checksum=" << header->checksum;
  }
  
  // Extract and display the readable payload content
  if (size > sizeof(MessageHeader)) {
    const char* payload = buffer + sizeof(MessageHeader);
    size_t payload_size = size - sizeof(MessageHeader);
    
    std::string readable_payload;
    for (size_t i = 0; i < payload_size && i < 256; ++i) {  // Limit to 256 chars
      char c = payload[i];
      if (c >= 32 && c <= 126) {  // Printable ASCII
        readable_payload += c;
      } else if (c == 0) {
        readable_payload += "<NULL>";
      } else {
        readable_payload += "<" + std::to_string(static_cast<unsigned char>(c)) + ">";
      }
    }
    
    if (payload_size > 256) {
      readable_payload += "...";
    }
    
    // ABSL_LOG(INFO) << "  Payload: " << readable_payload;
  }
}

}  // namespace message_utils

// AdaptiveBufferPool Implementation
// PHASE 1 OPTIMIZATION 3: UCX memory cleanup in BufferInfo destructor
AdaptiveBufferPool::BufferInfo::~BufferInfo() {
  // Only delete C++ allocated memory if data pointer is valid
  // UCX-registered memory is unmapped during shutdown and data is set to nullptr
  if (data != nullptr) {
    delete[] data;
  }
}

void AdaptiveBufferPool::Initialize() {
  absl::MutexLock lock(&mutex_);
  
  if (initialized_) {
    return;
  }
  
  // Pre-allocate initial buffers for each size category
  for (size_t i = 0; i < kInitialBuffers; ++i) {
    buffer_pool_.push_back(CreateBuffer(kSmallBufferSize));
    buffer_pool_.push_back(CreateBuffer(kMediumBufferSize));
    buffer_pool_.push_back(CreateBuffer(kLargeBufferSize));
  }
  
  initialized_ = true;
  // ABSL_LOG(INFO) << "AdaptiveBufferPool initialized with " << buffer_pool_.size() << " buffers";
}

char* AdaptiveBufferPool::AcquireBuffer(size_t required_size, size_t* actual_size) {
  absl::MutexLock lock(&mutex_);
  if (!initialized_) {
    Initialize();
  }
  
  // Legacy mode - exact current implementation
  size_t optimal_size = SelectBufferSize(required_size);
  
  // Try to find an available buffer of the optimal size
  for (size_t i = 0; i < buffer_pool_.size(); ++i) {
    auto& buffer = buffer_pool_[i];
    if (!buffer->in_use && buffer->size == optimal_size) {
      buffer->in_use = true;
      *actual_size = buffer->size;
      buffer_lookup_[buffer->data] = i; // Store correct index for fast lookup
      // ABSL_LOG(INFO) << "AdaptiveBufferPool: Reused buffer of size " << buffer->size;
      return buffer->data;
    }
  }
  
  // If no suitable buffer found and we haven't reached the limit, create a new one
  if (buffer_pool_.size() < kMaxBuffers) {
    auto new_buffer = CreateBuffer(optimal_size);
    char* buffer_ptr = new_buffer->data;
    new_buffer->in_use = true;
    buffer_lookup_[buffer_ptr] = buffer_pool_.size();
    buffer_pool_.push_back(std::move(new_buffer));
    *actual_size = optimal_size;
    // ABSL_LOG(INFO) << "AdaptiveBufferPool: Created new buffer of size " << optimal_size;
    return buffer_ptr;
  }
  
  // Fallback: find any available buffer that's large enough
  for (size_t i = 0; i < buffer_pool_.size(); ++i) {
    auto& buffer = buffer_pool_[i];
    if (!buffer->in_use && buffer->size >= required_size) {
      buffer->in_use = true;
      *actual_size = buffer->size;
      buffer_lookup_[buffer->data] = i; // Store correct index for fast lookup
      ABSL_LOG(WARNING) << "AdaptiveBufferPool: Using oversized buffer " << buffer->size 
                        << " for required size " << required_size;
      return buffer->data;
    }
  }
  
  // Last resort: create a temporary buffer (not pooled)
  ABSL_LOG(ERROR) << "AdaptiveBufferPool: Pool exhausted, creating temporary buffer";
  char* temp_buffer = new char[optimal_size];
  *actual_size = optimal_size;
  return temp_buffer;
}

void AdaptiveBufferPool::ReleaseBuffer(char* buffer) {
  absl::MutexLock lock(&mutex_);
  
  auto it = buffer_lookup_.find(buffer);
  if (it != buffer_lookup_.end()) {
    size_t index = it->second;
    if (index < buffer_pool_.size()) {
      buffer_pool_[index]->in_use = false;
      buffer_lookup_.erase(it);
      ABSL_LOG(INFO) << "AdaptiveBufferPool: Released buffer back to pool";
      return;
    }
  }
  
  // This was a temporary buffer, just delete it
  ABSL_LOG(WARNING) << "AdaptiveBufferPool: Deleting temporary buffer";
  delete[] buffer;
}

void AdaptiveBufferPool::CleanupOldBuffers(std::chrono::seconds max_age) {
  absl::MutexLock lock(&mutex_);
  
  auto now = std::chrono::steady_clock::now();
  size_t cleaned = 0;
  
  for (auto it = buffer_pool_.begin(); it != buffer_pool_.end();) {
    if (!(*it)->in_use && (now - (*it)->allocated_time) > max_age) {
      // Remove from lookup map
      buffer_lookup_.erase((*it)->data);
      it = buffer_pool_.erase(it);
      cleaned++;
    } else {
      ++it;
    }
  }
  
  if (cleaned > 0) {
    // ABSL_LOG(INFO) << "AdaptiveBufferPool: Cleaned up " << cleaned << " old buffers";
  }
}

AdaptiveBufferPool::Stats AdaptiveBufferPool::GetStats() const {
  absl::MutexLock lock(&mutex_);
  
  Stats stats{};
  stats.total_buffers = buffer_pool_.size();
  
  for (const auto& buffer : buffer_pool_) {
    if (buffer->in_use) {
      stats.buffers_in_use++;
    }
    
    stats.memory_usage_bytes += buffer->size;
    
    if (buffer->size == kSmallBufferSize) {
      stats.small_buffers++;
    } else if (buffer->size == kMediumBufferSize) {
      stats.medium_buffers++;
    } else if (buffer->size == kLargeBufferSize) {
      stats.large_buffers++;
    }
  }
  
  return stats;
}

void AdaptiveBufferPool::Shutdown() {
  absl::MutexLock lock(&mutex_);
  
  ABSL_LOG(INFO) << "AdaptiveBufferPool: Shutting down with " << buffer_pool_.size() << " buffers";
  
  // PHASE 1: Properly unmap UCX registered memory to satisfy UCX cleanup
  ABSL_LOG(INFO) << "Phase 1: Unmapping UCX registered memory regions";
  size_t buffers_in_use = 0;
  size_t ucx_regions_unmapped = 0;
  
  for (size_t i = 0; i < buffer_pool_.size(); ++i) {
    try {
      if (buffer_pool_[i]) {
        if (buffer_pool_[i]->in_use) {
          buffers_in_use++;
        }
        
        // CRITICAL: Unmap UCX memory if registered, and nullify data pointer
        if (buffer_pool_[i]->is_ucx_registered && buffer_pool_[i]->memory_handle != nullptr) {
          try {
            UcxManager& ucx_manager = UcxManager::Instance();
            ucp_context_h context = ucx_manager.GetContext();
            if (context != nullptr) {
              ABSL_LOG(INFO) << "Unmapping UCX region for buffer " << i;
              ucp_mem_unmap(context, buffer_pool_[i]->memory_handle);
              buffer_pool_[i]->memory_handle = nullptr;
              buffer_pool_[i]->is_ucx_registered = false;
              // CRITICAL: Set data pointer to nullptr to prevent double-free in destructor
              buffer_pool_[i]->data = nullptr;
              ucx_regions_unmapped++;
            }
          } catch (...) {
            ABSL_LOG(WARNING) << "Failed to unmap UCX region for buffer " << i;
          }
        }
      }
    } catch (...) {
      ABSL_LOG(WARNING) << "Buffer " << i << " appears corrupted - skipping";
    }
  }
  
  ABSL_LOG(INFO) << "Phase 1 complete: Unmapped " << ucx_regions_unmapped << " UCX memory regions";
  
  if (buffers_in_use > 0) {
    ABSL_LOG(WARNING) << "Found " << buffers_in_use << " buffers still in use";
  }
  
  // PHASE 2: Properly clear the buffer pool to prevent static destructor double-free
  // UCX memory regions are now properly unmapped, so we can safely clear the pool
  ABSL_LOG(INFO) << "Phase 2: Clearing buffer pool to prevent static destructor double-free";
  
  // Clear all BufferInfo objects to prevent their destructors from running during static cleanup
  buffer_pool_.clear();
  buffer_lookup_.clear();
  initialized_ = false;
  
  ABSL_LOG(INFO) << "AdaptiveBufferPool shutdown complete - UCX regions unmapped, buffer pool cleared";
}

size_t AdaptiveBufferPool::SelectBufferSize(size_t required_size) const {
  if (required_size <= kSmallBufferSize) {
    return kSmallBufferSize;
  } else if (required_size <= kMediumBufferSize) {
    return kMediumBufferSize;
  } else {
    return kLargeBufferSize;
  }
}

std::unique_ptr<AdaptiveBufferPool::BufferInfo> AdaptiveBufferPool::CreateBuffer(size_t size) {
  return std::make_unique<BufferInfo>(size);
}

// PHASE 1 OPTIMIZATION 3: UCX-aware buffer acquisition with memory registration
char* AdaptiveBufferPool::AcquireUcxBuffer(size_t required_size, size_t* actual_size, ucp_mem_h* mem_handle) {
  absl::MutexLock lock(&mutex_);
  if (!initialized_) {
    Initialize();
  }
  
  size_t optimal_size = SelectBufferSize(required_size);
  
  // First, try to find a UCX-registered buffer of the optimal size
  for (size_t i = 0; i < buffer_pool_.size(); ++i) {
    auto& buffer = buffer_pool_[i];
    if (!buffer->in_use && buffer->size == optimal_size && buffer->is_ucx_registered) {
      buffer->in_use = true;
      *actual_size = buffer->size;
      *mem_handle = buffer->memory_handle;
      buffer_lookup_[buffer->data] = i;
      return buffer->data;
    }
  }
  
  // If no UCX-registered buffer available, try to register an existing unregistered buffer
  for (size_t i = 0; i < buffer_pool_.size(); ++i) {
    auto& buffer = buffer_pool_[i];
    if (!buffer->in_use && buffer->size == optimal_size && !buffer->is_ucx_registered) {
      // Try to register this buffer with UCX
      if (TryRegisterBufferWithUcx(buffer.get())) {
        buffer->in_use = true;
        *actual_size = buffer->size;
        *mem_handle = buffer->memory_handle;
        buffer_lookup_[buffer->data] = i;
        return buffer->data;
      }
    }
  }
  
  // If no suitable buffer found and we haven't reached the limit, create and register a new one
  if (buffer_pool_.size() < kMaxBuffers) {
    auto new_buffer = CreateAndRegisterUcxBuffer(optimal_size);
    if (new_buffer && new_buffer->is_ucx_registered) {
      char* buffer_ptr = new_buffer->data;
      new_buffer->in_use = true;
      *actual_size = new_buffer->size;
      *mem_handle = new_buffer->memory_handle;
      buffer_lookup_[buffer_ptr] = buffer_pool_.size();
      buffer_pool_.push_back(std::move(new_buffer));
      return buffer_ptr;
    }
  }
  
  // Fallback: use regular buffer acquisition without UCX registration
  *mem_handle = nullptr;
  return AcquireBuffer(required_size, actual_size);
}

std::unique_ptr<AdaptiveBufferPool::BufferInfo> AdaptiveBufferPool::CreateAndRegisterUcxBuffer(size_t size) {
  auto buffer = std::make_unique<BufferInfo>(size);  
  if (TryRegisterBufferWithUcx(buffer.get())) {
    return buffer;
  }
  // Return unregistered buffer as fallback
  return buffer;
}

bool AdaptiveBufferPool::TryRegisterBufferWithUcx(BufferInfo* buffer) {
  // Try to register with UCX for optimal performance
  ucp_mem_map_params_t mem_params;
  memset(&mem_params, 0, sizeof(mem_params));
  mem_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mem_params.address = buffer->data;  // Register existing buffer
  mem_params.length = buffer->size;
  
  ucs_status_t status = ucp_mem_map(UcxManager::Instance().GetContext(),
                                   &mem_params, &buffer->memory_handle);
  if (status == UCS_OK) {
    buffer->is_ucx_registered = true;
    return true;
  }
  
  // Registration failed, buffer will work without registration
  buffer->is_ucx_registered = false;
  buffer->memory_handle = nullptr;
  return false;
}

// Phase 1: Conservative Buffer Enhancement Implementation

// UcxManager Implementation
UcxManager& UcxManager::Instance() {
  static UcxManager instance;
  return instance;
}

absl::Status UcxManager::Initialize() {
  {
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
                          UCP_PARAM_FIELD_TAG_SENDER_MASK |
                          UCP_PARAM_FIELD_REQUEST_SIZE |
                          UCP_PARAM_FIELD_REQUEST_INIT;
  
  // Use features that support local communication (shared memory) and networking
  ucp_params.features = UCP_FEATURE_TAG | 
                        UCP_FEATURE_WAKEUP |
                        UCP_FEATURE_AM |
                        UCP_FEATURE_RMA;  // Remote Memory Access for better performance
  
  ucp_params.tag_sender_mask = 0xF000000000000000ULL;  // Use upper 4 bits for message type
  ucp_params.request_size = 0;  // Use default request size
  ucp_params.request_init = nullptr;  // No request initialization
  
  // Create UCX context
  status = ucp_init(&ucp_params, config, &context_);
  ucp_config_release(config);
  
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to initialize UCX context: %s", 
                                               ucs_status_string(status)));
  }
  
  // Create UCX worker with socket support
  ucp_worker_params_t worker_params;
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE |
                             UCP_WORKER_PARAM_FIELD_CPU_MASK;
  worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
  // Use default CPU mask (all CPUs available)
  UCS_CPU_ZERO(&worker_params.cpu_mask);
  
  status = ucp_worker_create(context_, &worker_params, &worker_);
  if (status != UCS_OK) {
    ucp_cleanup(context_);
    context_ = nullptr;
    return absl::InternalError(absl::StrFormat("Failed to create UCX worker: %s", 
                                               ucs_status_string(status)));
  }
  
    initialized_ = true;
    // ABSL_LOG(INFO) << "UCX Manager initialized successfully with socket support";
  }  // Release mutex here
  
  // Initialize the adaptive buffer pool
  buffer_pool_.Initialize();
  
  // Start worker progress polling outside of mutex scope
  ABSL_LOG(INFO) << "Starting UCX worker progress task";
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
  
  // Post initial receive buffers using adaptive buffer management
  constexpr int initial_adaptive_buffers = 100;  // Increased to handle more concurrent requests
  ABSL_LOG(INFO) << "Posting " << initial_adaptive_buffers << " initial adaptive receive buffers";
  for (int i = 0; i < initial_adaptive_buffers; ++i) {
    PostAdaptiveServerReceiveNoLock();
  }
  
  return listener;
}

Result<ucp_ep_h> UcxManager::CreateClientEndpoint(const std::string& server_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
  // ABSL_LOG(INFO) << "Creating UCX client endpoint to: " << server_addr;
  
  // Force real UCX networking for all connections (localhost and remote)
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
  
  // Use socket address with proper field mask
  ep_params.field_mask = UCP_EP_PARAM_FIELD_SOCK_ADDR | UCP_EP_PARAM_FIELD_FLAGS;
  ep_params.sockaddr.addr = (const struct sockaddr*)&server_sockaddr;
  ep_params.sockaddr.addrlen = sizeof(server_sockaddr);
  ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  
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
  
  // Force progress to establish connection
  for (int i = 0; i < 10; ++i) {
    ucp_worker_progress(worker_);
  }
  ABSL_LOG(INFO) << "Forced initial progress after endpoint creation";
  
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
  
  // Complete async operation tracking (release throttling)
  async_manager_.CompleteOperation(AsyncOperationManager::Operation::READ, request_id);
}

// PHASE 1 OPTIMIZATION 4: Fast-path completion methods for direct callback processing
void UcxManager::CompletePendingOperationDirect(uint64_t request_id, absl::Status status) {
  // Try fast path first - attempt to acquire mutex without blocking
  if (mutex_.TryLock()) {
    auto it = pending_write_operations_.find(request_id);
    if (it != pending_write_operations_.end()) {
      // Fast path: complete directly in UCX context
      if (status.ok()) {
        it->second->promise.SetResult(absl::OkStatus());
      } else {
        it->second->promise.SetResult(status);
      }
      pending_write_operations_.erase(it);
      mutex_.Unlock();
      return;
    }
    mutex_.Unlock();
  }
  
  // Fallback to queued processing if mutex is contended or operation not found
  UcxCallbackDispatcher::ScheduleCallback([this, request_id, status]() {
    CompletePendingOperation(request_id, status);
  });
}

void UcxManager::CompletePendingReadOperationDirect(uint64_t request_id, kvstore::ReadResult result) {
  // Try fast path first - attempt to acquire mutex without blocking
  if (mutex_.TryLock()) {
    auto it = pending_read_operations_.find(request_id);
    if (it != pending_read_operations_.end()) {
      // Fast path: complete directly in UCX context
      it->second->promise.SetResult(std::move(result));
      pending_read_operations_.erase(it);
      mutex_.Unlock();
      
      // Complete async operation tracking (release throttling) 
      async_manager_.CompleteOperation(AsyncOperationManager::Operation::READ, request_id);
      return;
    }
    mutex_.Unlock();
  }
  
  // Fallback to queued processing if mutex is contended or operation not found
  UcxCallbackDispatcher::ScheduleCallback([this, request_id, result = std::move(result)]() mutable {
    CompletePendingReadOperation(request_id, std::move(result));
  });
}

uint64_t UcxManager::GenerateRequestId() {
  // PHASE 1 OPTIMIZATION 2: Lock-free atomic increment
  return next_request_id_.fetch_add(1, std::memory_order_relaxed);
}

void UcxManager::RegisterClientEndpoint(ucp_ep_h client_endpoint) {
  absl::MutexLock lock(&mutex_);
  client_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client endpoint, total clients: " << client_endpoints_.size();
}

void UcxManager::RegisterServerEndpoint(ucp_ep_h server_endpoint) {
  absl::MutexLock lock(&mutex_);
  server_endpoints_.push_back(server_endpoint);
  ABSL_LOG(INFO) << "Registered server endpoint, total server endpoints: " << server_endpoints_.size();
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
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
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
    // ABSL_LOG(INFO) << "Posted server receive buffer";
    
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
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
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

void UcxManager::PostAdaptiveServerReceive() {
  // Use adaptive buffer pool for optimal memory usage
  size_t actual_buffer_size;
  char* recv_buffer = buffer_pool_.AcquireBuffer(sizeof(MessageHeader) + 4194304, &actual_buffer_size);
  
  // Create user data structure to track buffer origin
  struct AdaptiveReceiveUserData {
    char* buffer;
    bool is_from_pool;
    size_t buffer_size;
  };
  
  AdaptiveReceiveUserData* user_data = new AdaptiveReceiveUserData{
    recv_buffer, 
    true,  // Mark as from pool for proper cleanup
    actual_buffer_size
  };
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = AdaptiveServerReceiveCallback;
  recv_params.user_data = user_data;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
  ucp_worker_h worker_handle;
  {
    absl::MutexLock lock(&mutex_);
    worker_handle = worker_;
  }
  
  if (!worker_handle) {
    ABSL_LOG(ERROR) << "Cannot post adaptive server receive: worker is null";
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_handle, recv_buffer, actual_buffer_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post adaptive server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    // ABSL_LOG(INFO) << "Posted adaptive server receive buffer of size " << actual_buffer_size;
    
    // Track the request for cleanup
    absl::MutexLock lock(&mutex_);
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
  }
}

void UcxManager::PostAdaptiveServerReceiveNoLock() {
  // Use adaptive buffer pool for optimal memory usage (mutex already held for main state)
  // Buffer pool has its own mutex, so it's safe to call from here
  size_t actual_buffer_size;
  char* recv_buffer = buffer_pool_.AcquireBuffer(sizeof(MessageHeader) + 4194304, &actual_buffer_size);
  
  // Create user data structure to track buffer origin
  struct AdaptiveReceiveUserData {
    char* buffer;
    bool is_from_pool;
    size_t buffer_size;
  };
  
  AdaptiveReceiveUserData* user_data = new AdaptiveReceiveUserData{
    recv_buffer, 
    true,  // Mark as from pool for proper cleanup
    actual_buffer_size
  };
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = AdaptiveServerReceiveCallback;
  recv_params.user_data = user_data;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
  if (!worker_) {
    ABSL_LOG(ERROR) << "Cannot post adaptive server receive: worker is null";
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_, recv_buffer, actual_buffer_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post adaptive server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    // ABSL_LOG(INFO) << "Posted adaptive server receive buffer of size " << actual_buffer_size;
    
    // Track the request for cleanup (mutex already held)
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    buffer_pool_.ReleaseBuffer(recv_buffer);
    delete user_data;
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
  
  // Use the safe ServerReadResponseTask with proper reference counting
  auto task = internal::MakeIntrusivePtr<ServerReadResponseTask>(
      client_endpoint, request_id, value);
  
  // Debug: Log what we're about to send
  uint32_t value_size = value.has_value() ? value->size() : 0;
  uint32_t status_code = value.has_value() ? 0 : 1;
  ABSL_LOG(INFO) << "Server sending read response: value_size=" << value_size 
                 << ", status_code=" << status_code;
  
  // Start the task - it will handle the entire send operation safely
  task->Start();
}

void UcxManager::SendWriteResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                   uint32_t status_code) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send write response: client endpoint is null";
    return;
  }
  
  // Use the safe ServerWriteResponseTask with proper reference counting
  auto task = internal::MakeIntrusivePtr<ServerWriteResponseTask>(
      client_endpoint, request_id, status_code);
  
  ABSL_LOG(INFO) << "Server sending write response: status_code=" << status_code;
  
  // Start the task - it will handle the entire send operation safely
  task->Start();
}

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_.load()) {
    ABSL_LOG(INFO) << "Worker progress task already running";
    return;
  }
  
  ABSL_LOG(INFO) << "Starting worker progress task";
  progress_task_running_.store(true);
  
  // ENHANCED: Use a joinable thread with proper lifetime management
  // This is safer than detached threads and allows for proper cleanup
  {
    absl::MutexLock lock(&mutex_);
    progress_thread_ = std::make_unique<std::thread>([this]() {
      WorkerProgressTask();
    });
  }
  
  // ABSL_LOG(INFO) << "UCX worker progress task started with enhanced thread management";
}

void UcxManager::WorkerProgressTask() {
  ABSL_LOG(INFO) << "UCX worker progress polling started";
  
  auto last_stats_time = std::chrono::steady_clock::now();
  const auto stats_interval = std::chrono::seconds(30);  // Report stats every 30 seconds
  
  while (true) {
    {
      absl::MutexLock lock(&mutex_);
      if (!initialized_ || !progress_task_running_) {
        break;
      }
      
      // Poll for progress
      unsigned progress_count = ucp_worker_progress(worker_);
      if (progress_count > 0) {
        ABSL_LOG(INFO) << "UCX progress made " << progress_count << " operations";
      }
    }
    
    // Periodically report buffer pool statistics
    auto now = std::chrono::steady_clock::now();
    if (now - last_stats_time >= stats_interval) {
      auto stats = buffer_pool_.GetStats();
      // ABSL_LOG(INFO) << "AdaptiveBufferPool Stats - Total: " << stats.total_buffers
      //              << ", In Use: " << stats.buffers_in_use
      //              << ", Memory: " << stats.memory_usage_bytes << " bytes"
      //              << ", Small: " << stats.small_buffers
      //              << ", Medium: " << stats.medium_buffers
      //              << ", Large: " << stats.large_buffers;
      
      // Clean up old unused buffers
      buffer_pool_.CleanupOldBuffers();
      
      last_stats_time = now;
    }
    
    // Sleep briefly to avoid busy waiting
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  
  // ABSL_LOG(INFO) << "UCX worker progress polling stopped";
}

void UcxManager::Shutdown() {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return;
  }
  
  ABSL_LOG(INFO) << "Starting UCX Manager shutdown";
  
  // ENHANCED: Stop the progress task and properly join the thread
  progress_task_running_.store(false);
  
  // ENHANCED: Shutdown the callback dispatcher to prevent deadlocks
  UcxCallbackDispatcher::Shutdown();
  
  // Join the progress thread if it exists
  if (progress_thread_ && progress_thread_->joinable()) {
    ABSL_LOG(INFO) << "Waiting for progress thread to complete...";
    // Release mutex before joining to avoid deadlock
    mutex_.Unlock();
    progress_thread_->join();
    mutex_.Lock();
    progress_thread_.reset();
    ABSL_LOG(INFO) << "Progress thread joined successfully";
  }
  
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
    if (client_side_endpoint) {
      ABSL_LOG(INFO) << "Destroying client-side endpoint";
      ucp_ep_destroy(client_side_endpoint);
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
  
  // CRITICAL: Clean up the adaptive buffer pool BEFORE setting shutdown flag
  // This ensures BufferInfo destructors can properly unmap UCX memory regions
  // while UCX context is still valid
  ABSL_LOG(INFO) << "Shutting down buffer pool with UCX context active";
  buffer_pool_.Shutdown();
  
  // NOW set shutdown flag to prevent any new buffer operations
  shutdown_started_.store(true);
  
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
    // Reinitialize client endpoint if null (connection persistence fix)
    if (!client_endpoint_) {
      // ABSL_LOG(INFO) << "WriteRemote reinitializing client endpoint for key '" << key << "'";
      auto& ucx_manager = UcxManager::Instance();
      auto endpoint_result = ucx_manager.CreateClientEndpoint(*spec_.remote_addr);
      if (!endpoint_result.ok()) {
        return endpoint_result.status();
      }
      client_endpoint_ = *endpoint_result;
    }
    
    // Use UCX networking for all connections
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    ABSL_LOG(INFO) << "WriteRemote sending data for key '" << key << "' with " << value.size() << " bytes to server";
    
    // PHASE 1 OPTIMIZATION 3: UCX-aware zero-copy message construction with registered memory
    size_t message_size;
    ucp_mem_h mem_handle;
    char* message_buffer = ZeroCopyMessageBuilder::BuildWriteMessageUcx(
        key, value, request_id, &message_size, &mem_handle);
    
    // Log message for debugging
    message_utils::LogMessageBuffer(message_buffer, message_size, "Client sending write request");
    
    ABSL_LOG(INFO) << "Sending UCX write request: key='" << key << "', value_size=" << value.size() << ", request_id=" << request_id;
    std::cout << "📤 CLIENT: Sending write request [ID:" << request_id << "]" << std::endl;
    
    // Create promise/future pair
    auto [promise, future] = PromiseFuturePair<void>::Make();
    
    // Register pending operation
    ucx_manager.RegisterPendingOperation(request_id, std::move(promise), MessageType::WRITE_REQUEST);
    
    // Post a receive buffer to get the write response
    PostWriteResponseReceive(request_id);
    
    // Send message using UCX tagged messaging
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));  // Initialize all fields to zero
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    
    // PHASE 1 OPTIMIZATION 3: Add memory handle for registered memory
    if (mem_handle != nullptr) {
      send_params.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMH;
      send_params.memh = mem_handle;
    }
    
    ucp_tag_t tag = UCX_TAG_WRITE_REQUEST;  // Use distinct tag for write operations
    
    // PHASE 1 OPTIMIZATION 3: Direct UCX send with optimized buffer
    char* stable_buffer = message_buffer;  // Already stable from UCX buffer pool or fallback
    
    // ABSL_LOG(INFO) << "About to send: buffer_ptr=" << static_cast<void*>(stable_buffer)
                  //  << ", size=" << message_size << ", tag=" << std::dec << tag;
    
    // Double-check the buffer contents before sending
    message_utils::LogMessageBuffer(stable_buffer, message_size, "UCX-optimized buffer before send");
    
    // Store the buffer pointer in user_data so we can free it in the callback
    struct SendUserData {
      uint64_t request_id;
      char* buffer_to_free;
      bool from_ucx_pool;  // Track if buffer came from UCX pool or regular allocation
    };
    SendUserData* user_data = new SendUserData{request_id, stable_buffer, (mem_handle != nullptr)};
    send_params.user_data = user_data;
    
    void* request = ucp_tag_send_nbx(client_endpoint_, stable_buffer, 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX send failed immediately: " << ucs_status_string(error_status);
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("UCX send failed: %s", 
              ucs_status_string(error_status))));
      // PHASE 1 OPTIMIZATION 3: Proper buffer cleanup based on source
      if (mem_handle != nullptr) {
        ucx_manager.GetBufferPool().ReleaseBuffer(stable_buffer);
      } else {
        delete[] stable_buffer;
      }
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately, but still wait for server response
      ABSL_LOG(INFO) << "UCX write request sent immediately for request_id=" << request_id;
      // DON'T complete here - wait for server response in ClientWriteResponseCallback
      // Clean up send resources only
      delete[] stable_buffer;
      delete user_data;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      ABSL_LOG(INFO) << "UCX write request in progress for request_id=" << request_id;
      // Resources will be cleaned up in SendCallback
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
    // High-performance async throttling to prevent UCX overload
    auto& ucx_manager = UcxManager::Instance();
    auto& async_manager = ucx_manager.GetAsyncManager();
    
    // If too many concurrent reads, wait briefly and retry
    if (async_manager.ShouldThrottleReads()) {
      // Return a future that will retry after a short delay
      auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
      
      // Schedule retry after brief delay (non-blocking)
      std::thread([promise = std::move(promise), this, key, options]() mutable {
        std::this_thread::sleep_for(std::chrono::microseconds(100)); // 0.1ms delay
        auto result = this->ReadRemote(key, options).result();
        promise.SetResult(result);
      }).detach();
      
      return future;
    }
    
    // Generate request ID and register operation
    uint64_t request_id = ucx_manager.GenerateRequestId();
    async_manager.RegisterOperation(AsyncOperationManager::Operation::READ, request_id);
    
    // Reinitialize client endpoint if null (connection persistence fix)
    if (!client_endpoint_) {
      // ABSL_LOG(INFO) << "ReadRemote reinitializing client endpoint for key '" << key << "'";
      auto& ucx_manager = UcxManager::Instance();
      auto endpoint_result = ucx_manager.CreateClientEndpoint(*spec_.remote_addr);
      if (!endpoint_result.ok()) {
        kvstore::ReadResult result;
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
        return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
      }
      client_endpoint_ = *endpoint_result;
    }
    
    // ABSL_LOG(INFO) << "ReadRemote sending read request for key '" << key << "' to server";
    
    // PHASE 1 OPTIMIZATION 1: Zero-copy read message construction
    size_t message_size;
    auto message_buffer = ZeroCopyMessageBuilder::BuildReadMessage(
        key, request_id, &message_size);
    
    // Log message buffer for debugging
    message_utils::LogMessageBuffer(message_buffer.get(), message_size, "Client sending read request");
    
    // ABSL_LOG(INFO) << "Sending UCX read request: key='" << key << "', request_id=" << request_id;
    // std::cout << "📤 CLIENT: Sending read request [ID:" << request_id << "] key='" << key << "'" << std::endl;
    
    // Create promise/future pair for read result
    auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
    
    // Register pending read operation
    ucx_manager.RegisterPendingReadOperation(request_id, std::move(promise));
    
    // Post a receive buffer to get the response
    PostReadResponseReceive(request_id);
    
    // Send read request using UCX tagged messaging
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));  // Initialize all fields to zero
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    
    // PHASE 1 OPTIMIZATION 1: Direct UCX send without additional copy
    char* stable_buffer = message_buffer.release();  // Transfer ownership to UCX
    
    // Store the buffer pointer in user_data so we can free it in the callback
    struct SendUserData {
      uint64_t request_id;
      char* buffer_to_free;
    };
    SendUserData* user_data = new SendUserData{request_id, stable_buffer};
    send_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_READ_REQUEST;  // Use distinct tag for read operations
    
    void* request = ucp_tag_send_nbx(client_endpoint_, stable_buffer, 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX read request send failed: " << ucs_status_string(error_status);
      
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
      // Clean up resources - ReadRemote uses regular allocation
      delete[] stable_buffer;
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately - response handling will complete the read
      ABSL_LOG(INFO) << "UCX read request sent immediately for request_id=" << request_id;
      // Clean up resources - ReadRemote uses regular allocation
      delete[] stable_buffer;
      delete user_data;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      // ABSL_LOG(INFO) << "UCX read request in progress for request_id=" << request_id;
      // Resources will be cleaned up in SendCallback
    }
    
    return future;
  }
  
  /// Post a receive buffer to get read response from server
  void PostReadResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the read response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 4 * 1024 * 1024 + 1024;  // 4MB + header overhead max response
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold both request_id and buffer
    struct ReadUserData {
      uint64_t request_id;
      char* buffer;
    };
    ReadUserData* user_data = new ReadUserData{request_id, recv_buffer};
    
    // Post non-blocking receive for read response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientReceiveCallback;
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_READ_RESPONSE;  // Use distinct tag for read responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      // ABSL_LOG(INFO) << "Posted client receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for read responses)
      // ABSL_LOG(INFO) << "Client receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
      delete user_data;
    }
  }
  
  /// Post a receive buffer to get write response from server
  void PostWriteResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the write response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 1024;  // Write response is small
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold both request_id and buffer (same pattern as read)
    struct WriteUserData {
      uint64_t request_id;
      char* buffer;
    };
    WriteUserData* user_data = new WriteUserData{request_id, recv_buffer};
    
    // Post non-blocking receive for write response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientWriteResponseCallback;
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_WRITE_RESPONSE;  // Use distinct tag for write responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client write response receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError("Failed to post write response receive"));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      // ABSL_LOG(INFO) << "Posted client write response receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for write responses)
      ABSL_LOG(INFO) << "Client write response receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
      delete user_data;
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
    // ABSL_LOG(INFO) << "Initializing UCX for client mode to " << *data_.remote_addr;
    
    // Create UCX endpoint to server
    auto endpoint_result = ucx_manager.CreateClientEndpoint(*data_.remote_addr);
    if (!endpoint_result.ok()) {
      return endpoint_result.status();
    }
    
    driver->client_endpoint_ = *endpoint_result;
    driver->is_server_mode_ = false;
    // ABSL_LOG(INFO) << "UCX client initialized successfully, connected to " << *data_.remote_addr;
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
 