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
#include "ucx_memory_pool.h"
#include "zero_copy_builder.h"

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

// Enhanced retry mechanism for UCX operations
class UcxRetryManager {
 public:
  static constexpr int kMaxRetries = 3;
  static constexpr absl::Duration kBaseDelay = absl::Milliseconds(100);
  static constexpr absl::Duration kMaxDelay = absl::Seconds(5);
  
  static bool IsRetriableError(ucs_status_t status) {
    return status == UCS_ERR_NO_MEMORY || 
           status == UCS_ERR_BUSY ||
           status == UCS_ERR_NO_RESOURCE;
  }
  
  static absl::Duration CalculateBackoffDelay(int attempt) {
    if (attempt >= kMaxRetries) {
      return absl::ZeroDuration();
    }
    // Exponential backoff with jitter
    auto delay = kBaseDelay * (1 << attempt);
    if (delay > kMaxDelay) {
      delay = kMaxDelay;
    }
    return delay;
  }
};

// Enhanced operation tracking for better error handling
struct UcxOperationTracker {
  uint64_t request_id;
  int attempt_count = 0;
  absl::Time start_time;
  absl::Time last_attempt_time;
  
  UcxOperationTracker(uint64_t id) 
    : request_id(id), start_time(absl::Now()), last_attempt_time(absl::Now()) {}
  
  bool ShouldRetry(ucs_status_t status) const {
    return attempt_count < UcxRetryManager::kMaxRetries && 
           UcxRetryManager::IsRetriableError(status);
  }
  
  absl::Duration GetNextRetryDelay() const {
    return UcxRetryManager::CalculateBackoffDelay(attempt_count);
  }
  
  void RecordAttempt() {
    attempt_count++;
    last_attempt_time = absl::Now();
  }
};

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

// Server-side task for handling delete range responses safely with reference counting
struct ServerDeleteRangeResponseTask : public internal::AtomicReferenceCount<ServerDeleteRangeResponseTask> {
  ucp_ep_h client_endpoint;
  uint64_t request_id;
  uint32_t status_code;
  uint32_t deleted_count;
  std::unique_ptr<UcxBuffer> send_buffer;
  
  ServerDeleteRangeResponseTask(ucp_ep_h ep, uint64_t id, uint32_t status, uint32_t count)
    : client_endpoint(ep), request_id(id), status_code(status), deleted_count(count) {}
  
  void Start() {
    send_buffer = UcxBuffer::Create(sizeof(DeleteRangeResponse));
    
    DeleteRangeResponse* response = reinterpret_cast<DeleteRangeResponse*>(send_buffer->data());
    response->header.magic_number = MESSAGE_MAGIC_NUMBER;
    response->header.type = MessageType::DELETE_RANGE_RESPONSE;
    response->header.key_length = 0;
    response->header.value_length = 0;
    response->header.request_id = request_id;
    response->header.checksum = 0;
    response->status_code = status_code;
    response->deleted_count = deleted_count;
    
    // Debug: Log what we're about to send
    ABSL_LOG(INFO) << "Server sending delete range response: status_code=" << status_code
                   << ", deleted_count=" << deleted_count
                   << ", request_id=" << request_id;
    
    // Send using UCX
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = ServerSendCallback;
    send_params.user_data = this;
    
    intrusive_ptr_increment(this);  // Add reference for the callback
    
    void* send_request = ucp_tag_send_nbx(client_endpoint, send_buffer->data(), 
                                         sizeof(DeleteRangeResponse), 
                                         UCX_TAG_DELETE_RANGE_RESPONSE, &send_params);
    
    if (UCS_PTR_IS_ERR(send_request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(send_request);
      ABSL_LOG(ERROR) << "Failed to send delete range response: " << ucs_status_string(error_status);
      intrusive_ptr_decrement(this);  // Cleanup
    } else if (send_request == nullptr) {
      // Send completed immediately
      ABSL_LOG(INFO) << "Delete range response sent immediately";
      intrusive_ptr_decrement(this);  // Cleanup
    }
  }
  
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* task = static_cast<ServerDeleteRangeResponseTask*>(user_data);
    ABSL_LOG(INFO) << "Server delete range response sent: " << ucs_status_string(status);
    
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

size_t RemoteDramStorage::RemoveRange(const std::string& prefix) {
  absl::MutexLock lock(&mutex_);
  size_t deleted_count = 0;
  
  auto it = storage_.begin();
  while (it != storage_.end()) {
    if (it->first.substr(0, prefix.length()) == prefix) {
      it = storage_.erase(it);
      deleted_count++;
    } else {
      ++it;
    }
  }
  
  return deleted_count;
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
void SendCallback(void* request, ucs_status_t status, void* user_data);

/// Multi-worker send callback for parallel processing (PHASE 2)
void MultiWorkerSendCallback(void* request, ucs_status_t status, void* user_data);

void SendCallback(void* request, ucs_status_t status, void* user_data) {
  // Handle the new user_data structure
  struct SendUserData {
    uint64_t request_id;
    char* buffer_to_free;
  };
  
  SendUserData* data = static_cast<SendUserData*>(user_data);
  uint64_t request_id = data->request_id;
  char* buffer_to_free = data->buffer_to_free;
  
  // ABSL_LOG(INFO) << "UCX send completed for request " << request_id 
  //                << " with status: " << ucs_status_string(status);
  
  if (status == UCS_OK) {
    std::cout << "✅ UCX: Send completed successfully [ID:" << request_id << "]" << std::endl;
  } else {
    std::cout << "❌ UCX: Send failed [ID:" << request_id << "] - " << ucs_status_string(status) << std::endl;
  }
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
  
  // ENHANCED: Use safer callback dispatcher instead of detached thread
  UcxCallbackDispatcher::ScheduleCallback([request_id, result_status, buffer_to_free, data]() {
    UcxManager::Instance().CompletePendingOperation(request_id, result_status);
    // Free the buffer and user_data after completing the operation
    delete[] buffer_to_free;
    delete data;
  });
  
  ucp_request_free(request);
}

/// Multi-worker send callback for parallel processing (PHASE 2)
void MultiWorkerSendCallback(void* request, ucs_status_t status, void* user_data) {
  struct MultiWorkerSendUserData {
    uint64_t request_id;
    char* buffer_to_free;
    ClientWorkerContext* worker_context;
  };
  
  auto* data = static_cast<MultiWorkerSendUserData*>(user_data);
  uint64_t request_id = data->request_id;
  char* buffer_to_free = data->buffer_to_free;
  ClientWorkerContext* worker_context = data->worker_context;
  
  if (status == UCS_OK) {
    std::cout << "✅ WORKER: Multi-worker send completed [ID:" << request_id 
              << ", Worker:" << worker_context << "]" << std::endl;
  } else {
    std::cout << "❌ WORKER: Multi-worker send failed [ID:" << request_id 
              << "] - " << ucs_status_string(status) << std::endl;
  }
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("Multi-worker UCX send failed: %s", ucs_status_string(status)));
  
  // Direct completion without dispatcher to eliminate serialization
  worker_context->CompletePendingWriteOperation(request_id, result_status);
  
  // Free resources
  delete[] buffer_to_free;
  delete data;
  ucp_request_free(request);
}

/// Fire-and-forget send callback for Phase 3 operations
static void FireAndForgetSendCallbackImpl(void* request, ucs_status_t status, void* user_data) {
  struct FireAndForgetUserData {
    uint64_t request_id;
    char* buffer_to_free;
    ClientWorkerContext* worker_context;
  };
  
  auto* data = static_cast<FireAndForgetUserData*>(user_data);
  uint64_t request_id = data->request_id;
  char* buffer_to_free = data->buffer_to_free;
  ClientWorkerContext* worker_context = data->worker_context;
  
  if (status == UCS_OK) {
    std::cout << "🚀 FIRE_AND_FORGET: Send completed [ID:" << request_id << "]" << std::endl;
  } else {
    std::cout << "❌ FIRE_AND_FORGET: Send failed [ID:" << request_id 
              << "] - " << ucs_status_string(status) << std::endl;
    ABSL_LOG(WARNING) << "Fire-and-forget write failed: " << ucs_status_string(status);
  }
  
  // Decrement outstanding operations counter for flow control
  if (worker_context) {
    worker_context->DecrementOutstanding();
  }
  
  // Free resources (no promise completion - already completed immediately)
  delete[] buffer_to_free;
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
      
      // std::cout << "📥 SERVER: Received write request [ID:" << header->request_id << "] key='" << key 
      //           << "' value='" << std::string(value) << "' - STORED SUCCESS" << std::endl;
      
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
        std::cout << "📤 SERVER: Found key '" << key << "' [ID:" << header->request_id << "] - sending response with " << value->size() << " bytes" << std::endl;
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
      
    } else if (header->type == MessageType::DELETE_RANGE_REQUEST) {
      // Handle delete range request
      const char* data_ptr = buffer + header_offset + sizeof(MessageHeader);
      std::string prefix(data_ptr, header->key_length);
      
      ABSL_LOG(INFO) << "Server received delete range request for prefix='" << prefix << "'";
      
      // Perform range deletion in storage
      auto& storage = UcxManager::Instance().GetStorage();
      size_t deleted_count = storage.RemoveRange(prefix);
      
      ABSL_LOG(INFO) << "Server deleted " << deleted_count << " keys with prefix '" << prefix << "'";
      
      // Send delete range response back to client using safe callback dispatcher
      uint64_t request_id = header->request_id;
      std::string prefix_copy = prefix;
      UcxCallbackDispatcher::ScheduleCallback([request_id, prefix_copy, deleted_count]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendDeleteRangeResponse(client_endpoint, request_id, 0, deleted_count);  // 0 = success
          ABSL_LOG(INFO) << "Sent delete range response for prefix: " << prefix_copy 
                         << ", deleted " << deleted_count << " keys";
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send delete range response for prefix: " << prefix_copy;
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
    
    ABSL_LOG(INFO) << "Client received read response: status_code=" << response->status_code
                   << ", value_len=" << response->header.value_length
                   << ", request_id=" << response->header.request_id;
    
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
        std::cout << "📥 CLIENT: Read response [ID:" << response->header.request_id << "] SUCCESS - received " << result.value.size() << " bytes" << std::endl;
      }
    } else {
      // Key not found or error
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      ABSL_LOG(INFO) << "Read result: key not found";
      std::cout << "❌ CLIENT: Read response [ID:" << response->header.request_id << "] KEY NOT FOUND" << std::endl;
    }
    
    // ENHANCED: Complete the pending read operation using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id, result = std::move(result)]() mutable {
      UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(result));
    });
    
    delete[] buffer;
    delete read_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive read response: " << ucs_status_string(status);
    
    // ENHANCED: Complete with error using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id]() {
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(error_result));
    });
    
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
    
    // ENHANCED: Complete the pending write operation using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id, result_status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, result_status);
    });
    
    delete[] buffer;
    delete write_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive write response: " << ucs_status_string(status);
    
    // ENHANCED: Complete with error using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id, status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("Failed to receive write response: %s", 
                                             ucs_status_string(status))));
    });
    
    delete[] buffer;
    delete write_data;
  }
  
  ucp_request_free(request);
}

/// Client-side callback for handling delete range responses
void ClientDeleteRangeResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  // Extract user data structure
  struct DeleteRangeUserData {
    uint64_t request_id;
    char* buffer;
  };
  DeleteRangeUserData* delete_data = static_cast<DeleteRangeUserData*>(user_data);
  uint64_t request_id = delete_data->request_id;
  char* buffer = delete_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(DeleteRangeResponse)) {
    // The data was received into the buffer we provided
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received delete range response");
    
    DeleteRangeResponse* response = reinterpret_cast<DeleteRangeResponse*>(response_data.data());
    
    ABSL_LOG(INFO) << "Client received delete range response: status_code=" << response->status_code
                   << ", deleted_count=" << response->deleted_count
                   << ", request_id=" << response->header.request_id;
    
    absl::Status result_status;
    if (response->status_code == 0) {
      // Success
      result_status = absl::OkStatus();
      ABSL_LOG(INFO) << "Delete range successful for request_id=" << response->header.request_id
                     << ", deleted " << response->deleted_count << " keys";
    } else {
      // Error
      result_status = absl::InternalError("Delete range failed on server");
      ABSL_LOG(ERROR) << "Delete range failed for request_id=" << response->header.request_id;
    }
    
    // Complete the pending delete range operation using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id, result_status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, result_status);
    });
    
    delete[] buffer;
    delete delete_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive delete range response: " << ucs_status_string(status);
    
    // Complete with error using safe callback dispatcher
    UcxCallbackDispatcher::ScheduleCallback([request_id, status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("Failed to receive delete range response: %s", 
                                             ucs_status_string(status))));
    });
    
    delete[] buffer;
    delete delete_data;
  }
  
  ucp_request_free(request);
}

/// Client-side callback for handling write responses from worker contexts
void ClientWorkerWriteResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  // ABSL_LOG(INFO) << "UCX worker write response received with status: " << ucs_status_string(status);
  
  // Extract user data structure (includes worker context)
  struct WorkerWriteUserData {
    uint64_t request_id;
    char* buffer;
    ClientWorkerContext* worker_context;
  };
  WorkerWriteUserData* write_data = static_cast<WorkerWriteUserData*>(user_data);
  uint64_t request_id = write_data->request_id;
  char* buffer = write_data->buffer;
  ClientWorkerContext* worker_context = write_data->worker_context;
  
  if (status == UCS_OK && info->length >= sizeof(WriteResponse)) {
    // The data was received into the buffer we provided
    // Copy to ensure we have valid data access (same pattern as read)
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content
    // ABSL_LOG(INFO) << "Worker received write response " << info->length << " bytes";
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Worker received write response");
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(response_data.data());
    
    // ABSL_LOG(INFO) << "Worker received write response: status_code=" << response->status_code
    //                << ", request_id=" << response->header.request_id;
    
    absl::Status result_status;
    if (response->status_code == 0) {
      // Success
      result_status = absl::OkStatus();
      // ABSL_LOG(INFO) << "Worker write successful for request_id=" << response->header.request_id;
      std::cout << "📥 WORKER: Write response [ID:" << response->header.request_id << "] SUCCESS" << std::endl;
    } else {
      // Error
      result_status = absl::InternalError("Write failed on server");
      ABSL_LOG(INFO) << "Worker write failed for request_id=" << response->header.request_id;
      std::cout << "❌ WORKER: Write response [ID:" << response->header.request_id << "] FAILED" << std::endl;
    }
    
    // Complete the pending write operation directly (no dispatcher needed for worker context)
    worker_context->CompletePendingWriteOperation(request_id, result_status);
    
    delete[] buffer;
    delete write_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive worker write response: " << ucs_status_string(status);
    
    // Complete with error directly
    worker_context->CompletePendingWriteOperation(request_id, 
        absl::InternalError(absl::StrFormat("Failed to receive worker write response: %s", 
                                           ucs_status_string(status))));
    
    delete[] buffer;
    delete write_data;
  }
  
  ucp_request_free(request);
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
      header->type != MessageType::READ_RESPONSE &&
      header->type != MessageType::DELETE_RANGE_REQUEST &&
      header->type != MessageType::DELETE_RANGE_RESPONSE) {
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

// ============================================================================
// UCX Memory Pool Implementation (PHASE 4)
// ============================================================================

UcxMemoryPool& UcxMemoryPool::Instance() {
  static UcxMemoryPool instance;
  return instance;
}

void UcxMemoryPool::Initialize() {
  Initialize(Config{});
}

void UcxMemoryPool::Initialize(const Config& config) {
  absl::MutexLock lock(&mutex_);
  
  if (initialized_) {
    return;  // Already initialized
  }
  
  config_ = config;
  
  // Initialize small buffer pool
  small_buffers_.reserve(config.small_pool_size);
  for (size_t i = 0; i < config.small_pool_size; ++i) {
    small_buffers_.push_back(std::make_unique<PooledBuffer>(config.small_buffer_size));
  }
  
  // Initialize medium buffer pool
  medium_buffers_.reserve(config.medium_pool_size);
  for (size_t i = 0; i < config.medium_pool_size; ++i) {
    medium_buffers_.push_back(std::make_unique<PooledBuffer>(config.medium_buffer_size));
  }
  
  // Initialize large buffer pool
  large_buffers_.reserve(config.large_pool_size);
  for (size_t i = 0; i < config.large_pool_size; ++i) {
    large_buffers_.push_back(std::make_unique<PooledBuffer>(config.large_buffer_size));
  }
  
  initialized_ = true;
  ABSL_LOG(INFO) << "UCX Memory Pool initialized: "
                 << config.small_pool_size << " small (" << config.small_buffer_size << "B), "
                 << config.medium_pool_size << " medium (" << config.medium_buffer_size << "B), "
                 << config.large_pool_size << " large (" << config.large_buffer_size << "B)";
}

std::vector<std::unique_ptr<UcxMemoryPool::PooledBuffer>>& UcxMemoryPool::SelectPool(size_t required_size) {
  absl::MutexLock lock(&mutex_);
  
  if (required_size <= config_.small_buffer_size) {
    return small_buffers_;
  } else if (required_size <= config_.medium_buffer_size) {
    return medium_buffers_;
  } else {
    return large_buffers_;
  }
}

const std::vector<std::unique_ptr<UcxMemoryPool::PooledBuffer>>& UcxMemoryPool::SelectPool(size_t required_size) const {
  absl::MutexLock lock(&mutex_);
  
  if (required_size <= config_.small_buffer_size) {
    return small_buffers_;
  } else if (required_size <= config_.medium_buffer_size) {
    return medium_buffers_;
  } else {
    return large_buffers_;
  }
}

std::unique_ptr<UcxMemoryPool::BufferLease> UcxMemoryPool::AcquireBuffer(size_t required_size) {
  total_allocations_.fetch_add(1, std::memory_order_relaxed);
  
  // Ensure pool is initialized
  {
    absl::MutexLock lock(&mutex_);
    if (!initialized_) {
      Initialize();
    }
  }
  
  // Select appropriate pool
  auto& pool = SelectPool(required_size);
  
  // Try to find available buffer in pool
  {
    absl::MutexLock lock(&mutex_);
    for (auto& buffer : pool) {
      bool expected = false;
      if (buffer->in_use.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
        // Successfully acquired buffer from pool
        pool_hits_.fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<BufferLease>(buffer->data, buffer->size, buffer.get());
      }
    }
  }
  
  // No available buffer in pool - create new one
  pool_misses_.fetch_add(1, std::memory_order_relaxed);
  
  // Determine actual size to allocate
  size_t alloc_size;
  {
    absl::MutexLock lock(&mutex_);
    if (required_size <= config_.small_buffer_size) {
      alloc_size = config_.small_buffer_size;
    } else if (required_size <= config_.medium_buffer_size) {
      alloc_size = config_.medium_buffer_size;
    } else if (required_size <= config_.large_buffer_size) {
      alloc_size = config_.large_buffer_size;
    } else {
      // Very large buffer - allocate exact size
      alloc_size = required_size;
    }
  }
  
  auto new_buffer = std::make_unique<PooledBuffer>(alloc_size);
  new_buffer->in_use.store(true, std::memory_order_release);
  
  BufferLease* lease = new BufferLease(new_buffer->data, new_buffer->size, new_buffer.get());
  
  // Add new buffer to appropriate pool for future reuse
  {
    absl::MutexLock lock(&mutex_);
    if (alloc_size == config_.small_buffer_size) {
      small_buffers_.push_back(std::move(new_buffer));
    } else if (alloc_size == config_.medium_buffer_size) {
      medium_buffers_.push_back(std::move(new_buffer));
    } else if (alloc_size == config_.large_buffer_size) {
      large_buffers_.push_back(std::move(new_buffer));
    } else {
      // Very large buffer - don't add to pool, will be deleted when lease expires
      new_buffer.release();  // Transfer ownership to lease
    }
  }
  
  return std::unique_ptr<BufferLease>(lease);
}

UcxMemoryPool::PoolStats UcxMemoryPool::GetStats() const {
  absl::MutexLock lock(&mutex_);
  
  PoolStats stats = {};
  stats.small_pool_size = small_buffers_.size();
  stats.medium_pool_size = medium_buffers_.size();
  stats.large_pool_size = large_buffers_.size();
  
  // Count buffers in use
  for (const auto& buffer : small_buffers_) {
    if (buffer->in_use.load()) stats.small_in_use++;
  }
  for (const auto& buffer : medium_buffers_) {
    if (buffer->in_use.load()) stats.medium_in_use++;
  }
  for (const auto& buffer : large_buffers_) {
    if (buffer->in_use.load()) stats.large_in_use++;
  }
  
  stats.total_allocations = total_allocations_.load();
  stats.pool_hits = pool_hits_.load();
  stats.pool_misses = pool_misses_.load();
  
  return stats;
}

// ============================================================================
// Zero-Copy Message Builder Implementation (PHASE 4)
// ============================================================================

void ZeroCopyMessageBuilder::CopyCordToBuffer(const absl::Cord& cord, char* dest_buffer) {
  if (cord.size() == 0) {
    return;
  }
  
  // PHASE 4: Optimize based on Cord's internal structure
  if (cord.size() <= 4096) {
    // For small cords, try to get flat representation
    auto flat_view = cord.TryFlat();
    if (flat_view.has_value()) {
      // Cord is already flat - single memcpy
      std::memcpy(dest_buffer, flat_view->data(), flat_view->size());
      return;
    }
  }
  
  // Fallback to manual copying for complex cords
  size_t pos = 0;
  for (absl::string_view chunk : cord.Chunks()) {
    std::memcpy(dest_buffer + pos, chunk.data(), chunk.size());
    pos += chunk.size();
  }
}

std::unique_ptr<ZeroCopyMessageBuilder::MessageBuffer> 
ZeroCopyMessageBuilder::BuildWriteMessage(const kvstore::Key& key, const absl::Cord& value, uint64_t request_id) {
  
  size_t total_size = sizeof(MessageHeader) + key.size() + value.size();
  
  // Get buffer from pool (NO ALLOCATION if available)
  auto buffer_lease = UcxMemoryPool::Instance().AcquireBuffer(total_size);
  
  // Construct message directly in UCX buffer (NO INTERMEDIATE COPIES)
  char* buffer = buffer_lease->data();
  MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
  char* payload = buffer + sizeof(MessageHeader);
  
  // Copy key directly to final location (SINGLE COPY)
  std::memcpy(payload, key.data(), key.size());
  
  // Copy value directly to final location (SINGLE COPY)
  CopyCordToBuffer(value, payload + key.size());
  
  // Initialize header directly with payload checksum
  message_utils::InitializeMessageHeader(
      header, MessageType::WRITE_REQUEST,
      static_cast<uint32_t>(key.size()),
      static_cast<uint32_t>(value.size()),
      request_id, payload);
  
  return std::make_unique<MessageBuffer>(
      std::move(buffer_lease), header, payload, total_size);
}

std::unique_ptr<ZeroCopyMessageBuilder::MessageBuffer> 
ZeroCopyMessageBuilder::BuildReadMessage(const kvstore::Key& key, uint64_t request_id) {
  
  size_t total_size = sizeof(MessageHeader) + key.size();
  
  // Get buffer from pool
  auto buffer_lease = UcxMemoryPool::Instance().AcquireBuffer(total_size);
  
  // Construct message directly in UCX buffer
  char* buffer = buffer_lease->data();
  MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
  char* payload = buffer + sizeof(MessageHeader);
  
  // Copy key directly to final location (SINGLE COPY)
  std::memcpy(payload, key.data(), key.size());
  
  // Initialize header for read request
  message_utils::InitializeMessageHeader(
      header, MessageType::READ_REQUEST,
      static_cast<uint32_t>(key.size()),
      0,  // No value for read request
      request_id, payload);
  
  return std::make_unique<MessageBuffer>(
      std::move(buffer_lease), header, payload, total_size);
}

/// Zero-copy callback for UCX send completion
void ZeroCopyWriteCallback(void* request, ucs_status_t status, void* user_data) {
  // user_data contains ZeroCopyUserData with MessageBuffer
  auto* data = static_cast<ZeroCopyUserData*>(user_data);
  uint64_t request_id = data->request_id;
  ClientWorkerContext* worker_context = data->worker_context;
  
  if (status == UCS_OK) {
    std::cout << "⚡ ZERO_COPY: Write completed [ID:" << request_id << "]" << std::endl;
  } else {
    std::cout << "❌ ZERO_COPY: Write failed [ID:" << request_id 
              << "] - " << ucs_status_string(status) << std::endl;
    ABSL_LOG(WARNING) << "Zero-copy write failed: " << ucs_status_string(status);
  }
  
  // Decrement outstanding operations counter for flow control
  if (worker_context) {
    worker_context->DecrementOutstanding();
  }
  
  // Buffer automatically returns to pool when MessageBuffer destructor runs
  delete data;
  ucp_request_free(request);
}

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
  
  // Configure UCX for high-throughput rendezvous protocol
  ABSL_LOG(INFO) << "Configuring UCX for high-throughput rendezvous protocol";
  
  // Set rendezvous threshold to 8KB for optimal large message handling
  status = ucp_config_modify(config, "RNDV_THRESH", "8192");
  if (status != UCS_OK) {
    ABSL_LOG(WARNING) << "Failed to set UCX RNDV_THRESH: " << ucs_status_string(status);
  } else {
    ABSL_LOG(INFO) << "UCX RNDV_THRESH set to 8192 bytes";
  }
  
  // Enable memory registration cache for better performance
  status = ucp_config_modify(config, "REG_CACHE", "y");
  if (status != UCS_OK) {
    ABSL_LOG(WARNING) << "Failed to set UCX REG_CACHE: " << ucs_status_string(status);
  } else {
    ABSL_LOG(INFO) << "UCX REG_CACHE enabled";
  }
  
  // Increase memory bandwidth utilization (use single rail for optimal performance)
  status = ucp_config_modify(config, "MAX_RNDV_RAILS", "1");
  if (status != UCS_OK) {
    ABSL_LOG(WARNING) << "Failed to set UCX MAX_RNDV_RAILS: " << ucs_status_string(status);
  } else {
    ABSL_LOG(INFO) << "UCX MAX_RNDV_RAILS set to 1";
  }
  
  // Disable memory hooks for better stability under high load
  status = ucp_config_modify(config, "MEM_HOOKS", "no");
  if (status != UCS_OK) {
    ABSL_LOG(WARNING) << "Failed to set UCX MEM_HOOKS: " << ucs_status_string(status);
  } else {
    ABSL_LOG(INFO) << "UCX MEM_HOOKS disabled for stability";
  }
  
  // Set UCX context parameters with tagged messaging support and high-throughput optimizations
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | 
                          UCP_PARAM_FIELD_TAG_SENDER_MASK |
                          UCP_PARAM_FIELD_REQUEST_SIZE |
                          UCP_PARAM_FIELD_REQUEST_INIT |
                          UCP_PARAM_FIELD_MT_WORKERS_SHARED |
                          UCP_PARAM_FIELD_ESTIMATED_NUM_EPS;
  
  // Use features that support local communication (shared memory) and networking
  ucp_params.features = UCP_FEATURE_TAG | 
                        UCP_FEATURE_WAKEUP |
                        UCP_FEATURE_AM |
                        UCP_FEATURE_RMA |  // Remote Memory Access for better performance
                        UCP_FEATURE_STREAM;  // Stream support for large data transfers
  
  ucp_params.tag_sender_mask = 0xF000000000000000ULL;  // Use upper 4 bits for message type
  ucp_params.request_size = 0;  // Use default request size
  ucp_params.request_init = nullptr;  // No request initialization
  ucp_params.mt_workers_shared = 1;  // Enable multi-threaded worker sharing for better performance
  ucp_params.estimated_num_eps = 16;  // Estimate higher number of endpoints for better resource allocation
  
  // Create UCX context
  status = ucp_init(&ucp_params, config, &context_);
  ucp_config_release(config);
  
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to initialize UCX context: %s", 
                                               ucs_status_string(status)));
  }
  
  // Create UCX worker with socket support and high-throughput optimizations
  ucp_worker_params_t worker_params;
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE |
                             UCP_WORKER_PARAM_FIELD_CPU_MASK |
                             UCP_WORKER_PARAM_FIELD_EVENTS;
  worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
  worker_params.events = UCP_WAKEUP_TAG_SEND | UCP_WAKEUP_TAG_RECV | UCP_WAKEUP_RMA;
  // Use default CPU mask (all CPUs available)
  UCS_CPU_ZERO(&worker_params.cpu_mask);
  
  status = ucp_worker_create(context_, &worker_params, &worker_);
  if (status != UCS_OK) {
    ucp_cleanup(context_);
    context_ = nullptr;
    return absl::InternalError(absl::StrFormat("Failed to create UCX worker: %s", 
                                               ucs_status_string(status)));
  }
  
  // Update atomic worker handle for lock-free progress polling
  worker_atomic_.store(worker_, std::memory_order_release);
  
    initialized_ = true;
    ABSL_LOG(INFO) << "UCX Manager initialized successfully with socket support";
  }  // Release mutex here
  
  // Start worker progress polling outside of mutex scope
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
  constexpr int num_preposted_receives = 2000;
  // ABSL_LOG(INFO) << "Posting " << num_preposted_receives << " initial receive buffers";
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
  
  // Register this endpoint for cleanup
  RegisterClientSideEndpointNoLock(client_endpoint);
  
  return client_endpoint;
}

// Create endpoint using specific worker context to avoid protocol conflicts
Result<ucp_ep_h> CreateWorkerSpecificEndpoint(ucp_worker_h worker, const std::string& server_addr) {
  if (!worker) {
    return absl::FailedPreconditionError("Worker context not available");
  }
  
  ABSL_LOG(INFO) << "Creating worker-specific UCX endpoint to: " << server_addr;
  
  // Parse server address  
  std::string host;
  uint16_t port;
  size_t colon_pos = server_addr.find(':');
  if (colon_pos == std::string::npos) {
    return absl::InvalidArgumentError("Invalid server address format. Expected host:port");
  }
  
  host = server_addr.substr(0, colon_pos);
  try {
    port = static_cast<uint16_t>(std::stoi(server_addr.substr(colon_pos + 1)));
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid port in server address: %s", e.what()));
  }
  
  // Configure socket address for TCP connection
  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  server_sockaddr.sin_family = AF_INET;
  server_sockaddr.sin_port = htons(port);
  
  if (inet_pton(AF_INET, host.c_str(), &server_sockaddr.sin_addr) <= 0) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid IP address: %s", host));
  }
  
  // Configure endpoint parameters for socket connection
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_SOCK_ADDR;
  ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  ep_params.sockaddr.addr = reinterpret_cast<const struct sockaddr*>(&server_sockaddr);
  ep_params.sockaddr.addrlen = sizeof(server_sockaddr);
  
  ABSL_LOG(INFO) << "Creating worker-specific endpoint with socket connection";
  ABSL_LOG(INFO) << "  Server address: " << server_addr;
  ABSL_LOG(INFO) << "  Socket family: " << server_sockaddr.sin_family;
  ABSL_LOG(INFO) << "  Socket port: " << ntohs(server_sockaddr.sin_port);
  ABSL_LOG(INFO) << "  Worker: " << worker;
  
  // Create the client endpoint using the worker-specific context
  ucp_ep_h client_endpoint;
  ucs_status_t status = ucp_ep_create(worker, &ep_params, &client_endpoint);
  
  ABSL_LOG(INFO) << "Worker-specific endpoint creation result: " << ucs_status_string(status) << " (" << status << ")";
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create worker-specific UCX endpoint: " << ucs_status_string(status);
    return absl::InternalError(absl::StrFormat(
        "Failed to create UCX client endpoint: %s", ucs_status_string(status)));
  }
  
  ABSL_LOG(INFO) << "Worker-specific UCX client endpoint created successfully to " << server_addr;
  
  return client_endpoint;
}

// ============================================================================
// ClientWorkerContext Implementation
// ============================================================================

void ClientWorkerContext::RegisterPendingWriteOperation(uint64_t request_id, Promise<void> promise) {
  absl::MutexLock lock(&pending_ops_mutex);
  auto op = std::make_unique<PendingWriteOperation>(request_id, std::move(promise));
  pending_write_operations[request_id] = std::move(op);
}

void ClientWorkerContext::RegisterPendingReadOperation(uint64_t request_id, Promise<kvstore::ReadResult> promise) {
  absl::MutexLock lock(&pending_ops_mutex);
  auto op = std::make_unique<PendingReadOperation>(request_id, std::move(promise));
  pending_read_operations[request_id] = std::move(op);
}

void ClientWorkerContext::CompletePendingWriteOperation(uint64_t request_id, absl::Status status) {
  absl::MutexLock lock(&pending_ops_mutex);
  auto it = pending_write_operations.find(request_id);
  if (it != pending_write_operations.end()) {
    if (status.ok()) {
      it->second->promise.SetResult(absl::OkStatus());
    } else {
      it->second->promise.SetResult(status);
    }
    pending_write_operations.erase(it);
  }
}

void ClientWorkerContext::CompletePendingReadOperation(uint64_t request_id, kvstore::ReadResult result) {
  absl::MutexLock lock(&pending_ops_mutex);
  auto it = pending_read_operations.find(request_id);
  if (it != pending_read_operations.end()) {
    it->second->promise.SetResult(std::move(result));
    pending_read_operations.erase(it);
  }
}

void ClientWorkerContext::WorkerProgressTask() {
  ABSL_LOG(INFO) << "ClientWorkerContext progress polling started with optimized 1μs polling";
  
  int idle_count = 0;
  while (active.load(std::memory_order_acquire)) {
    if (worker) {
      // Poll for progress without mutex - lock-free operation
      unsigned progress_made = ucp_worker_progress(worker);
      
      if (progress_made > 0) {
        idle_count = 0;  // Reset idle counter on activity
      } else {
        idle_count++;
        // Adaptive polling: busy wait initially, then micro-sleep
        if (idle_count > 1000) {
          // High idle count: use 1μs sleep (100x faster than original 100μs)
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        } else if (idle_count > 100) {
          // Medium idle count: yield CPU
          std::this_thread::yield();
        }
        // Low idle count: busy poll for ultra-low latency
      }
    } else {
      // No worker available, sleep briefly
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }
  
  ABSL_LOG(INFO) << "ClientWorkerContext progress polling stopped";
}

Future<TimestampedStorageGeneration> ClientWorkerContext::WriteFireAndForget(
    const kvstore::Key& key, const absl::Cord& value, ucp_ep_h endpoint) {
  
  // PHASE 3: Flow control - wait if too many outstanding operations
  while (!CanSendOperation()) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  
  // Generate request ID (for logging/debugging only)
  uint64_t request_id = GenerateRequestId();
  
  ABSL_LOG(INFO) << "PHASE3: Fire-and-forget write for key '" << key 
                 << "' with " << value.size() << " bytes, request_id=" << request_id;
  
  // Convert absl::Cord to std::string for content copying
  std::string value_content = std::string(value);
  
  // Create write message with proper content copying
  size_t message_size = sizeof(MessageHeader) + key.size() + value_content.size();
  std::vector<char> message_buffer(message_size);
  
  // Copy key and value data
  char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
  std::memcpy(data_ptr, key.data(), key.size());
  std::memcpy(data_ptr + key.size(), value_content.data(), value_content.size());
  
  // Initialize message header
  MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
  message_utils::InitializeMessageHeader(header, MessageType::WRITE_REQUEST, 
                                        static_cast<uint32_t>(key.size()),
                                        static_cast<uint32_t>(value_content.size()),
                                        request_id, data_ptr);
  
  std::cout << "🚀 FIRE_AND_FORGET: Sending write request [ID:" << request_id << "]" << std::endl;
  
  // Increment outstanding operations counter for flow control
  IncrementOutstanding();
  
  // Prepare UCX send parameters
  ucp_request_param_t send_params;
  memset(&send_params, 0, sizeof(send_params));
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  send_params.cb.send = FireAndForgetSendCallbackImpl;
  
  // Allocate stable buffer for UCX send
  char* stable_buffer = new char[message_size];
  std::memcpy(stable_buffer, message_buffer.data(), message_size);
  
  // User data for callback with worker context
  struct FireAndForgetUserData {
    uint64_t request_id;
    char* buffer_to_free;
    ClientWorkerContext* worker_context;
  };
  auto* user_data = new FireAndForgetUserData{request_id, stable_buffer, this};
  send_params.user_data = user_data;
  
  // Send the message using UCX
  ucp_tag_t tag = UCX_TAG_WRITE_REQUEST;
  void* request = ucp_tag_send_nbx(endpoint, stable_buffer, message_size, tag, &send_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ucs_status_t error_status = UCS_PTR_STATUS(request);
    ABSL_LOG(ERROR) << "Fire-and-forget UCX send failed: " << ucs_status_string(error_status);
    DecrementOutstanding();
    delete[] stable_buffer;
    delete user_data;
    
    // Return immediate error
    return absl::InternalError(absl::StrFormat("UCX send failed: %s", 
        ucs_status_string(error_status)));
  } else if (request == nullptr) {
    // Send completed immediately
    ABSL_LOG(INFO) << "Fire-and-forget write sent immediately for request_id=" << request_id;
    DecrementOutstanding();
    delete[] stable_buffer;
    delete user_data;
  }
  // else: Send is in progress, will complete asynchronously via FireAndForgetSendCallback
  
  // PHASE 3: IMMEDIATE COMPLETION - Don't wait for server ACK!
  TimestampedStorageGeneration result;
  result.generation = StorageGeneration::FromString("fire_and_forget");
  result.time = absl::Now();
  
  return MakeReadyFuture<TimestampedStorageGeneration>(result);
}

Future<TimestampedStorageGeneration> ClientWorkerContext::WriteZeroCopy(
    const kvstore::Key& key, const absl::Cord& value, ucp_ep_h endpoint) {
  
  // PHASE 4: Flow control - wait if too many outstanding operations
  while (!CanSendOperation()) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  
  // Generate request ID (for logging/debugging only)
  uint64_t request_id = GenerateRequestId();
  
  // ABSL_LOG(INFO) << "PHASE4: Zero-copy write for key '" << key 
                //  << "' with " << value.size() << " bytes, request_id=" << request_id;
  
  // Build message directly in UCX buffer (NO INTERMEDIATE COPIES)
  auto message = ZeroCopyMessageBuilder::BuildWriteMessage(key, value, request_id);
  
  std::cout << "⚡ ZERO_COPY: Sending write request [ID:" << request_id << "]" << std::endl;
  
  // Increment outstanding operations counter for flow control
  IncrementOutstanding();
  
  // Prepare UCX send parameters
  ucp_request_param_t send_params;
  memset(&send_params, 0, sizeof(send_params));
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  send_params.cb.send = ZeroCopyWriteCallback;
  
  // User data for callback with zero-copy buffer
  auto* user_data = new ZeroCopyUserData(request_id, std::move(message), this);
  send_params.user_data = user_data;
  
  // Send directly from zero-copy buffer
  ucp_tag_t tag = UCX_TAG_WRITE_REQUEST;
  void* request = ucp_tag_send_nbx(endpoint, user_data->message_buffer->buffer_lease->data(), 
                                   user_data->message_buffer->total_size, tag, &send_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ucs_status_t error_status = UCS_PTR_STATUS(request);
    ABSL_LOG(ERROR) << "Zero-copy UCX send failed: " << ucs_status_string(error_status);
    DecrementOutstanding();
    delete user_data;
    
    // Return immediate error
    return absl::InternalError(absl::StrFormat("UCX send failed: %s", 
        ucs_status_string(error_status)));
  } else if (request == nullptr) {
    // Send completed immediately
    ABSL_LOG(INFO) << "Zero-copy write sent immediately for request_id=" << request_id;
    DecrementOutstanding();
    delete user_data;
  }
  // else: Send is in progress, will complete asynchronously via ZeroCopyWriteCallback
  
  // PHASE 4: IMMEDIATE COMPLETION - Don't wait for server ACK!
  TimestampedStorageGeneration result;
  result.generation = StorageGeneration::FromString("zero_copy_write");
  result.time = absl::Now();
  
  return MakeReadyFuture<TimestampedStorageGeneration>(result);
}

void ClientWorkerContext::Shutdown() {
  // Stop progress thread
  active.store(false, std::memory_order_release);
  if (progress_thread && progress_thread->joinable()) {
    progress_thread->join();
  }
  
  // Complete any pending operations with cancelled status
  {
    absl::MutexLock lock(&pending_ops_mutex);
    for (auto& [id, op] : pending_write_operations) {
      op->promise.SetResult(absl::CancelledError("ClientWorkerContext shutting down"));
    }
    pending_write_operations.clear();
    
    for (auto& [id, op] : pending_read_operations) {
      kvstore::ReadResult cancelled_result;
      cancelled_result.state = kvstore::ReadResult::kMissing;
      cancelled_result.stamp.generation = StorageGeneration::NoValue();
      cancelled_result.stamp.time = absl::Now();
      op->promise.SetResult(std::move(cancelled_result));
    }
    pending_read_operations.clear();
  }
  
  // Process any remaining UCX events before cleanup
  if (worker) {
    // Brief processing to clear any pending operations
    for (int i = 0; i < 5; ++i) {
      ucp_worker_progress(worker);
    }
  }
  
  // Cleanup UCX endpoint and worker
  if (endpoint) {
    ucp_ep_destroy(endpoint);
    endpoint = nullptr;
  }
  
  if (worker) {
    ucp_worker_destroy(worker);
    worker = nullptr;
  }
}

// ============================================================================
// UcxManager Implementation with Multi-Worker Support  
// ============================================================================

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

// ============================================================================
// Multi-Worker Pool Implementation
// ============================================================================

absl::Status UcxManager::InitializeWorkerPool(size_t num_workers) {
  absl::MutexLock lock(&mutex_);
  
  if (worker_pool_initialized_) {
    return absl::OkStatus(); // Already initialized
  }
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX context must be initialized before worker pool");
  }
  
  ABSL_LOG(INFO) << "Initializing worker pool with " << num_workers << " workers for parallel operations";
  
  client_workers_.reserve(num_workers);
  
  for (size_t i = 0; i < num_workers; ++i) {
    auto worker_context = std::make_unique<ClientWorkerContext>();
    
    // Create UCX worker for this client context
    ucp_worker_params_t worker_params;
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
    
    ucs_status_t status = ucp_worker_create(context_, &worker_params, &worker_context->worker);
    if (status != UCS_OK) {
      return absl::InternalError(absl::StrFormat("Failed to create UCX worker %zu: %s", 
                                                 i, ucs_status_string(status)));
    }
    
    // Start progress thread for this worker
    worker_context->progress_thread = std::make_unique<std::thread>(
      [worker_context = worker_context.get()]() {
        worker_context->WorkerProgressTask();
      }
    );
    
    client_workers_.push_back(std::move(worker_context));
    ABSL_LOG(INFO) << "Created worker " << i << " with dedicated progress thread";
  }
  
  worker_pool_initialized_ = true;
  ABSL_LOG(INFO) << "Worker pool initialization complete with " << num_workers << " parallel workers";
  
  return absl::OkStatus();
}

ClientWorkerContext* UcxManager::GetWorkerForNewOperation() {
  if (!worker_pool_initialized_ || client_workers_.empty()) {
    ABSL_LOG(WARNING) << "Worker pool not initialized, falling back to single-worker mode";
    return nullptr;
  }
  
  // Round-robin load balancing
  size_t index = next_worker_index_.fetch_add(1, std::memory_order_relaxed) % client_workers_.size();
  return client_workers_[index].get();
}

ClientWorkerContext* UcxManager::SelectWorkerForKey(const kvstore::Key& key) {
  if (!worker_pool_initialized_ || client_workers_.empty()) {
    return nullptr;
  }
  
  // Key-based sharding for better cache locality (optional optimization)
  size_t index = std::hash<std::string>{}(std::string(key)) % client_workers_.size();
  return client_workers_[index].get();
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
  // HPC OPTIMIZATION: Post receives for write, read, and delete range requests for maximum throughput
  absl::MutexLock lock(&mutex_);
  PostSingleServerReceiveNoLock(UCX_TAG_WRITE_REQUEST);
  PostSingleServerReceiveNoLock(UCX_TAG_READ_REQUEST);
  PostSingleServerReceiveNoLock(UCX_TAG_DELETE_RANGE_REQUEST);
}

void UcxManager::PostServerReceiveNoLock() {
  // HPC OPTIMIZATION: Post receives for write, read, and delete range requests for maximum throughput
  PostSingleServerReceiveNoLock(UCX_TAG_WRITE_REQUEST);
  PostSingleServerReceiveNoLock(UCX_TAG_READ_REQUEST);
  PostSingleServerReceiveNoLock(UCX_TAG_DELETE_RANGE_REQUEST);
}

void UcxManager::PostSingleServerReceiveNoLock(ucp_tag_t target_tag) {
  // Allocate buffer for incoming message
  constexpr size_t max_message_size = 64 * 1024;  // 64KB max message
  char* recv_buffer = new char[max_message_size];
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ServerReceiveCallback;
  recv_params.user_data = recv_buffer;
  
  // HPC OPTIMIZATION: Use specific tags for high-performance message matching
  ucp_tag_t tag = target_tag;           // Match specific request type
  ucp_tag_t tag_mask = UCX_TAG_MASK;    // Use proper tag mask for performance - server handles all message types
  
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
    // ABSL_LOG(INFO) << "Posted server receive buffer";
    
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

void UcxManager::SendDeleteRangeResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                         uint32_t status_code, uint32_t deleted_count) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send delete range response: client endpoint is null";
    return;
  }
  
  // Use the safe ServerDeleteRangeResponseTask with proper reference counting
  auto task = internal::MakeIntrusivePtr<ServerDeleteRangeResponseTask>(
      client_endpoint, request_id, status_code, deleted_count);
  
  ABSL_LOG(INFO) << "Server sending delete range response: status_code=" << status_code
                 << ", deleted_count=" << deleted_count;
  
  // Start the task - it will handle the entire send operation safely
  task->Start();
}

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_.load()) {
    return;
  }
  
  progress_task_running_.store(true);
  
  // ENHANCED: Use a joinable thread with proper lifetime management
  // This is safer than detached threads and allows for proper cleanup
  {
    absl::MutexLock lock(&mutex_);
    progress_thread_ = std::make_unique<std::thread>([this]() {
      WorkerProgressTask();
    });
  }
  
  ABSL_LOG(INFO) << "UCX worker progress task started with enhanced thread management";
}

void UcxManager::WorkerProgressTask() {
  ABSL_LOG(INFO) << "UCX worker progress polling started with optimized 1μs polling";
  
  int idle_count = 0;
  while (progress_task_running_.load(std::memory_order_acquire)) {
    ucp_worker_h current_worker = worker_atomic_.load(std::memory_order_acquire);
    
    if (current_worker) {
      // Poll for progress without mutex - lock-free operation
      unsigned progress_made = ucp_worker_progress(current_worker);
      
      if (progress_made > 0) {
        idle_count = 0;  // Reset idle counter on activity
      } else {
        idle_count++;
        // Adaptive polling: busy wait initially, then micro-sleep
        if (idle_count > 1000) {
          // High idle count: use 1μs sleep (100x faster than original 100μs)
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        } else if (idle_count > 100) {
          // Medium idle count: yield CPU
          std::this_thread::yield();
        }
        // Low idle count: busy poll for ultra-low latency
      }
    } else {
      // No worker available, sleep briefly
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }
  
  ABSL_LOG(INFO) << "UCX worker progress polling stopped";
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
  
  // Shutdown worker pool
  if (worker_pool_initialized_) {
    ABSL_LOG(INFO) << "Shutting down worker pool with " << client_workers_.size() << " workers";
    for (auto& worker_context : client_workers_) {
      if (worker_context) {
        worker_context->Shutdown();
      }
    }
    client_workers_.clear();
    worker_pool_initialized_ = false;
    ABSL_LOG(INFO) << "Worker pool shutdown complete";
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
  
  // Process any remaining UCX events before destroying worker
  if (worker_) {
    // Brief processing to clear any pending operations
    for (int i = 0; i < 10; ++i) {
      ucp_worker_progress(worker_);
    }
  }
  
  // Clean up UCX resources in reverse order of creation
  if (worker_) {
    ABSL_LOG(INFO) << "Destroying UCX worker";
    // Clear atomic worker handle before destroying
    worker_atomic_.store(nullptr, std::memory_order_release);
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
    if (is_server_mode_) {
      // Server mode: delete locally
      return DeleteRangeLocal(range);
    } else {
      // Client mode: send to remote server
      return DeleteRangeRemote(range);
    }
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
  ucp_ep_h client_endpoint_ = nullptr;  // UCX endpoint for client mode (legacy)
  std::string server_addr_;             // Server address for worker pool connections
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

  Future<const void> DeleteRangeLocal(const KeyRange& range) {
    auto& storage = UcxManager::Instance().GetStorage();
    
    if (range.empty()) {
      return MakeReadyFuture();
    }
    
    // For local delete range, we need to convert KeyRange to prefix
    // In most cases, we'll treat the range as a prefix deletion
    std::string prefix = range.inclusive_min;
    size_t deleted_count = storage.RemoveRange(prefix);
    
    ABSL_LOG(INFO) << "DeleteRangeLocal deleted " << deleted_count 
                   << " keys with prefix '" << prefix << "'";
    
    return MakeReadyFuture();
  }

  Future<const void> DeleteRangeRemote(const KeyRange& range) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "DeleteRangeRemote called but client_endpoint is null";
      return absl::InternalError("Client endpoint not available");
    }

    if (range.empty()) {
      return MakeReadyFuture();
    }

    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    // Convert KeyRange to prefix for deletion
    std::string prefix = range.inclusive_min;
    
    ABSL_LOG(INFO) << "DeleteRangeRemote sending delete range request for prefix '" 
                   << prefix << "'";

    // Create delete range message
    size_t message_size = sizeof(MessageHeader) + prefix.size();
    std::vector<char> message_buffer(message_size);
    
    // Copy prefix data
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, prefix.data(), prefix.size());
    
    // Initialize message header
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    message_utils::InitializeMessageHeader(header, MessageType::DELETE_RANGE_REQUEST, 
                                          static_cast<uint32_t>(prefix.size()),
                                          0,  // no value for delete range
                                          request_id, data_ptr);

    // Create promise for the operation
    auto [promise, future] = PromiseFuturePair<void>::Make();
    
    // Register pending operation
    ucx_manager.RegisterPendingOperation(request_id, std::move(promise), MessageType::DELETE_RANGE_REQUEST);
    
    // Post receive buffer for delete range response
    PostDeleteRangeResponseReceive(request_id);
    
    // Send message using UCX
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    send_params.user_data = reinterpret_cast<void*>(request_id);

    void* send_request = ucp_tag_send_nbx(client_endpoint_, message_buffer.data(), 
                                         message_size, UCX_TAG_DELETE_RANGE_REQUEST, &send_params);

    if (UCS_PTR_IS_ERR(send_request)) {
      ucs_status_t status = UCS_PTR_STATUS(send_request);
      ABSL_LOG(ERROR) << "Failed to send delete range request: " << ucs_status_string(status);
      return absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
    } else if (send_request != nullptr) {
      // Request is in progress
      ABSL_LOG(INFO) << "Delete range request sent successfully [ID:" << request_id << "]";
    }
    
    return std::move(future);
  }
  
  Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key, 
                                                   const absl::Cord& value) {
    auto& ucx_manager = UcxManager::Instance();
    
    // PHASE 2: Try to get a worker from the pool for parallel processing
    ClientWorkerContext* worker_context = ucx_manager.GetWorkerForNewOperation();
    
    if (worker_context && worker_context->worker) {
      // Use multi-worker parallel processing
      return WriteRemoteWithWorker(key, value, worker_context);
    } else {
      // Fallback to legacy single-worker mode
      ABSL_LOG(WARNING) << "Using legacy single-worker mode for key '" << key << "'";
      return WriteRemoteLegacy(key, value);
    }
  }

  Future<TimestampedStorageGeneration> WriteRemoteWithWorker(const kvstore::Key& key, 
                                                            const absl::Cord& value,
                                                            ClientWorkerContext* worker_context) {
    // PHASE 4: Use zero-copy writes for maximum performance
    // ABSL_LOG(INFO) << "PHASE4: Using zero-copy write for key '" << key 
                  //  << "' with " << value.size() << " bytes";
    
    // Initialize memory pool on first use
    UcxMemoryPool::Instance().Initialize();
    
    // Use shared client endpoint with zero-copy buffer management
    return worker_context->WriteZeroCopy(key, value, client_endpoint_);
  }

  Future<TimestampedStorageGeneration> WriteRemoteLegacy(const kvstore::Key& key, 
                                                        const absl::Cord& value) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "WriteRemoteLegacy called but client_endpoint is null for key '" << key << "'";
      return absl::InternalError("Client endpoint not available");
    }
    
    // Use UCX networking for all connections
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    // ABSL_LOG(INFO) << "WriteRemote sending data for key '" << key << "' with " << value.size() << " bytes to server";
    
    // Create write message
    size_t message_size = sizeof(MessageHeader) + key.size() + value.size();
    std::vector<char> message_buffer(message_size);
    
    // Copy key and value data first
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    // Handle absl::Cord properly
    std::string value_str = std::string(value);
    std::memcpy(data_ptr + key.size(), value_str.data(), value_str.size());
    
    // Initialize message header with integrity verification
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    message_utils::InitializeMessageHeader(header, MessageType::WRITE_REQUEST, 
                                          static_cast<uint32_t>(key.size()),
                                          static_cast<uint32_t>(value.size()),
                                          request_id, data_ptr);
    
    // Verify header was initialized correctly
    // ABSL_LOG(INFO) << "Client initialized header: magic=" << std::dec << header->magic_number
    //                << ", type=" << static_cast<uint32_t>(header->type)
    //                << ", key_len=" << header->key_length
    //                << ", value_len=" << header->value_length
    //                << ", request_id=" << header->request_id
    //                << ", checksum=" << header->checksum;
    
    // Log message for debugging
    message_utils::LogMessageBuffer(message_buffer.data(), message_size, "Client sending write request");
    
    // ABSL_LOG(INFO) << "Sending UCX write request: key='" << key << "', value='" << value_str << "', request_id=" << request_id;
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
    send_params.user_data = reinterpret_cast<void*>(request_id);
    
    ucp_tag_t tag = UCX_TAG_WRITE_REQUEST;  // Use distinct tag for write operations
    
    // Allocate a stable buffer that will survive until the send completes
    char* stable_buffer = new char[message_size];
    std::memcpy(stable_buffer, message_buffer.data(), message_size);
    
    // ABSL_LOG(INFO) << "About to send: buffer_ptr=" << static_cast<void*>(stable_buffer)
                  //  << ", size=" << message_size << ", tag=" << std::dec << tag;
    
    // Double-check the buffer contents before sending
    message_utils::LogMessageBuffer(stable_buffer, message_size, "Final buffer before UCX send");
    
    // Store the buffer pointer in user_data so we can free it in the callback
    struct SendUserData {
      uint64_t request_id;
      char* buffer_to_free;
    };
    SendUserData* user_data = new SendUserData{request_id, stable_buffer};
    send_params.user_data = user_data;
    
    void* request = ucp_tag_send_nbx(client_endpoint_, stable_buffer, 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX send failed immediately: " << ucs_status_string(error_status);
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("UCX send failed: %s", 
              ucs_status_string(error_status))));
      // Clean up resources
      delete[] stable_buffer;
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
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "ReadRemote called but client_endpoint is null for key '" << key << "'";
      
      kvstore::ReadResult result;
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // Use UCX networking for all connections
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    // ABSL_LOG(INFO) << "ReadRemote sending read request for key '" << key << "' to server";
    
    // Create read request message
    size_t message_size = sizeof(MessageHeader) + key.size();
    std::vector<char> message_buffer(message_size);
    
    // Copy key data first
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    // Initialize message header with integrity verification
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    message_utils::InitializeMessageHeader(header, MessageType::READ_REQUEST, 
                                          static_cast<uint32_t>(key.size()),
                                          0,  // No value in read request
                                          request_id, data_ptr);
    
    // Log message buffer for debugging
    message_utils::LogMessageBuffer(message_buffer.data(), message_size, "Client sending read request");
    
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
    
    // Allocate a stable buffer that will survive until the send completes
    char* stable_buffer = new char[message_size];
    std::memcpy(stable_buffer, message_buffer.data(), message_size);
    
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
      // Clean up resources
      delete[] stable_buffer;
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately - response handling will complete the read
      ABSL_LOG(INFO) << "UCX read request sent immediately for request_id=" << request_id;
      // Clean up resources
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
    constexpr size_t max_response_size = 64 * 1024;  // 64KB max response
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

  /// Post a receive buffer to get delete range response from server
  void PostDeleteRangeResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the delete range response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 1024;  // Delete range response is small
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold both request_id and buffer
    struct DeleteRangeUserData {
      uint64_t request_id;
      char* buffer;
    };
    DeleteRangeUserData* user_data = new DeleteRangeUserData{request_id, recv_buffer};
    
    // Post non-blocking receive for delete range response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientDeleteRangeResponseCallback;
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_DELETE_RANGE_RESPONSE;  // Use distinct tag for delete range responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client delete range response receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError("Failed to post delete range response receive"));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      ABSL_LOG(INFO) << "Posted client delete range response receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for delete range responses)
      ABSL_LOG(INFO) << "Client delete range response receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
      delete user_data;
    }
  }
  
  /// Post a receive buffer to get write response from server using worker-specific UCX context
  void PostWorkerWriteResponseReceive(ClientWorkerContext* worker_context, uint64_t request_id) {
    if (!worker_context || !worker_context->worker) {
      ABSL_LOG(ERROR) << "Invalid worker context for PostWorkerWriteResponseReceive";
      return;
    }
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 1024;  // Write response is small
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold request_id, buffer, and worker_context
    struct WorkerWriteUserData {
      uint64_t request_id;
      char* buffer;
      ClientWorkerContext* worker_context;
    };
    WorkerWriteUserData* user_data = new WorkerWriteUserData{request_id, recv_buffer, worker_context};
    
    // Post non-blocking receive for write response using worker's UCX context
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientWorkerWriteResponseCallback;  // Need worker-specific callback
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_WRITE_RESPONSE;  // Use distinct tag for write responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    // FIX: Use the shared worker that handles the client endpoint connection
    // The server sends responses back through the original connection endpoint
    auto& ucx_manager = UcxManager::Instance();
    ucp_worker_h shared_worker = ucx_manager.GetWorker();  // Use the main worker that handles client_endpoint_
    void* request = ucp_tag_recv_nbx(shared_worker, recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post worker write response receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      worker_context->CompletePendingWriteOperation(request_id, 
          absl::InternalError("Failed to post worker write response receive"));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      ABSL_LOG(INFO) << "Posted worker write response receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for write responses)
      ABSL_LOG(INFO) << "Worker write response receive completed immediately for request_id=" << request_id;
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
    ABSL_LOG(INFO) << "Initializing UCX for client mode to " << *data_.remote_addr;
    
    // Create UCX endpoint to server
    auto endpoint_result = ucx_manager.CreateClientEndpoint(*data_.remote_addr);
    if (!endpoint_result.ok()) {
      return endpoint_result.status();
    }
    
    driver->client_endpoint_ = *endpoint_result;
    driver->server_addr_ = *data_.remote_addr;  // Store server address for worker pool
    driver->is_server_mode_ = false;
    
    // PHASE 2: Initialize worker pool for parallel processing
    auto worker_pool_status = ucx_manager.InitializeWorkerPool(4);  // 4 workers default
    if (!worker_pool_status.ok()) {
      ABSL_LOG(WARNING) << "Failed to initialize worker pool: " << worker_pool_status
                        << ". Falling back to single-worker mode.";
    } else {
      ABSL_LOG(INFO) << "Multi-worker pool initialized for parallel processing";
    }
    
    ABSL_LOG(INFO) << "UCX client initialized successfully, connected to " << *data_.remote_addr;
  }
  
  return kvstore::DriverPtr(driver.get());
}

Result<kvstore::Spec> ParseRemoteDramUrl(std::string_view url) {
  return absl::UnimplementedError(
      "remote_dram driver URL parsing not yet implemented");
}

}  // namespace

/// Fire-and-forget send callback for Phase 3 operations
void FireAndForgetSendCallback(void* request, ucs_status_t status, void* user_data) {
  FireAndForgetSendCallbackImpl(request, status, user_data);
}

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