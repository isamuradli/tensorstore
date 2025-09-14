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
#include "unified_buffer_manager.h"

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
// PERFORMANCE OPTIMIZATION: Multi-threaded callback dispatcher
// Eliminates single-thread bottleneck for concurrent callback processing  
class UcxCallbackDispatcher {
 private:
  // Multi-threaded configuration
  // CONCURRENCY LIMIT: Reduced worker threads for stability
  static constexpr int NUM_WORKER_THREADS = 2;  // Reduced from 4 for stability
  static constexpr size_t BATCH_SIZE = 4;       // Reduced from 8 for stability
  
  // Thread-safe queue for each worker thread (reduces contention)
  struct ThreadSafeQueue {
    std::queue<std::function<void()>> queue;
    std::mutex mutex;
    std::condition_variable cv;
    
    template<typename Fn>
    void emplace(Fn&& fn) {
      std::lock_guard<std::mutex> lock(mutex);
      queue.emplace(std::forward<Fn>(fn));
      cv.notify_one();
    }
    
    bool try_pop(std::function<void()>& callback) {
      std::unique_lock<std::mutex> lock(mutex);
      if (queue.empty()) {
        return false;
      }
      callback = std::move(queue.front());
      queue.pop();
      return true;
    }
    
    void wait_and_pop(std::function<void()>& callback) {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [this] { return !queue.empty() || !UcxCallbackDispatcher::worker_running_.load(); });
      if (!queue.empty()) {
        callback = std::move(queue.front());
        queue.pop();
      }
    }
    
    void process_batch(size_t max_batch_size) {
      std::vector<std::function<void()>> batch;
      batch.reserve(max_batch_size);
      
      // Collect batch of callbacks
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return !queue.empty() || !UcxCallbackDispatcher::worker_running_.load(); });
        
        size_t count = 0;
        while (!queue.empty() && count < max_batch_size) {
          batch.emplace_back(std::move(queue.front()));
          queue.pop();
          ++count;
        }
      }
      
      // Execute batch outside of mutex
      for (auto& callback : batch) {
        if (callback) {
          callback();
        }
      }
    }
  };
  
  static std::array<ThreadSafeQueue, NUM_WORKER_THREADS> callback_queues_;
  static std::array<std::thread, NUM_WORKER_THREADS> worker_threads_;
  static std::atomic<bool> worker_running_;
  static std::atomic<size_t> next_queue_;  // Round-robin distribution
  
  // Static cleanup helper to ensure threads are joined before program exit
  struct CleanupHelper {
    ~CleanupHelper() {
      if (worker_running_.load()) {
        UcxCallbackDispatcher::Shutdown();
      }
    }
  };
  static CleanupHelper cleanup_helper_;
  
  static void WorkerLoop(size_t thread_id) {
    while (worker_running_.load()) {
      // Process callbacks in batches for better efficiency
      callback_queues_[thread_id].process_batch(BATCH_SIZE);
    }
    
    // Process any remaining callbacks after shutdown signal
    std::function<void()> callback;
    while (callback_queues_[thread_id].try_pop(callback)) {
      if (callback) {
        callback();
      }
    }
  }
  
 public:
  static void Initialize() {
    bool expected = false;
    if (worker_running_.compare_exchange_strong(expected, true)) {
      // Start all worker threads
      for (size_t i = 0; i < NUM_WORKER_THREADS; ++i) {
        worker_threads_[i] = std::thread(WorkerLoop, i);
      }
    }
  }
  
  static void Shutdown() {
    worker_running_.store(false);
    
    // Notify all worker threads to wake up and check shutdown condition
    for (auto& queue : callback_queues_) {
      queue.cv.notify_all();
    }
    
    // Join all worker threads
    for (auto& thread : worker_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }
  
  template<typename Fn>
  static void ScheduleCallback(Fn&& fn) {
    // Initialize workers if not already running
    Initialize();
    
    // Round-robin distribution across worker threads to balance load
    size_t queue_idx = next_queue_.fetch_add(1, std::memory_order_relaxed) % NUM_WORKER_THREADS;
    callback_queues_[queue_idx].emplace(std::forward<Fn>(fn));
  }
};

// Static member definitions for multi-threaded UcxCallbackDispatcher
std::array<UcxCallbackDispatcher::ThreadSafeQueue, UcxCallbackDispatcher::NUM_WORKER_THREADS> UcxCallbackDispatcher::callback_queues_;
std::array<std::thread, UcxCallbackDispatcher::NUM_WORKER_THREADS> UcxCallbackDispatcher::worker_threads_;
std::atomic<bool> UcxCallbackDispatcher::worker_running_{false};
std::atomic<size_t> UcxCallbackDispatcher::next_queue_{0};
UcxCallbackDispatcher::CleanupHelper UcxCallbackDispatcher::cleanup_helper_;


// UcxNativeBuffer implementation

std::unique_ptr<UcxNativeBuffer> UcxNativeBuffer::Create(ucp_context_h context, size_t size) {
  return std::make_unique<UcxNativeBuffer>(context, size);
}

UcxNativeBuffer::UcxNativeBuffer(ucp_context_h context, size_t size)
  : context_(context), size_(size), data_(nullptr), mem_handle_(nullptr) {

  // Use UCX native memory allocation with registration
  ucp_mem_map_params_t mem_params;
  mem_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                         UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                         UCP_MEM_MAP_PARAM_FIELD_FLAGS;
  mem_params.address = nullptr;  // Let UCX allocate
  mem_params.length = size;
  mem_params.flags = UCP_MEM_MAP_ALLOCATE;  // Allocate and register

  ucs_status_t status = ucp_mem_map(context_, &mem_params, &mem_handle_);
  if (status == UCS_OK) {
    // Get the allocated address
    ucp_mem_attr_t mem_attr;
    mem_attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS;

    ucs_status_t attr_status = ucp_mem_query(mem_handle_, &mem_attr);
    if (attr_status == UCS_OK) {
      data_ = static_cast<char*>(mem_attr.address);
    } else {
      ABSL_LOG(ERROR) << "Failed to query UCX memory attributes: "
                      << ucs_status_string(attr_status);
      ucp_mem_unmap(context_, mem_handle_);
      mem_handle_ = nullptr;
    }
  } else {
    ABSL_LOG(ERROR) << "Failed to allocate UCX memory: " << ucs_status_string(status);
    mem_handle_ = nullptr;
  }
}

UcxNativeBuffer::~UcxNativeBuffer() {
  if (mem_handle_ != nullptr) {
    ucp_mem_unmap(context_, mem_handle_);
  }
}

UcxNativeBuffer::UcxNativeBuffer(UcxNativeBuffer&& other) noexcept
  : context_(other.context_), size_(other.size_),
    data_(other.data_), mem_handle_(other.mem_handle_) {
  other.data_ = nullptr;
  other.mem_handle_ = nullptr;
}

UcxNativeBuffer& UcxNativeBuffer::operator=(UcxNativeBuffer&& other) noexcept {
  if (this != &other) {
    if (mem_handle_ != nullptr) {
      ucp_mem_unmap(context_, mem_handle_);
    }
    context_ = other.context_;
    size_ = other.size_;
    data_ = other.data_;
    mem_handle_ = other.mem_handle_;
    other.data_ = nullptr;
    other.mem_handle_ = nullptr;
  }
  return *this;
}

// Server-side task for handling read responses safely with reference counting
struct ServerReadResponseTask : public internal::AtomicReferenceCount<ServerReadResponseTask> {
  ucp_ep_h client_endpoint;
  ucp_context_h context;
  uint64_t request_id;
  std::optional<absl::Cord> value;
  std::unique_ptr<UcxNativeBuffer> send_buffer;
  
  ServerReadResponseTask(ucp_ep_h ep, ucp_context_h ctx, uint64_t id, std::optional<absl::Cord> val)
    : client_endpoint(ep), context(ctx), request_id(id), value(std::move(val)) {}
  
  void Start() {
    // Create response message
    uint32_t status_code = value.has_value() ? 0 : 1;
    uint32_t value_size = value.has_value() ? value->size() : 0;
    size_t total_size = sizeof(ReadResponse) + value_size;
    
    // Use UCX native memory allocation and registration
    send_buffer = UcxNativeBuffer::Create(context, total_size);
    if (!send_buffer->is_valid()) {
      ABSL_LOG(ERROR) << "Failed to allocate UCX native buffer for read response";
      return;  // Task will be cleaned up when it goes out of scope
    }
    
    ReadResponse* response = reinterpret_cast<ReadResponse*>(send_buffer->data());
    response->header.magic_number = MESSAGE_MAGIC_NUMBER;
    response->header.type = MessageType::READ_RESPONSE;
    response->header.key_length = 0;
    response->header.value_length = value_size;
    response->header.request_id = request_id;
    response->status_code = status_code;
    
    if (value.has_value() && value_size > 0) {
      // Direct Cord chunk processing eliminates string conversion
      response->header.checksum = 0;  // Checksums disabled  // Checksums disabled
      
      // Direct Cord-to-buffer copy without intermediate string allocation
      size_t offset = 0;
      char* dest_buffer = send_buffer->data() + sizeof(ReadResponse);
      for (absl::string_view chunk : value->Chunks()) {
        std::memcpy(dest_buffer + offset, chunk.data(), chunk.size());
        offset += chunk.size();
      }
    } else {
      response->header.checksum = 0;  // Checksums disabled  // Checksums disabled
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
      intrusive_ptr_decrement(this);  // Cleanup reference we added for callback
    } else if (request == nullptr) {
      // CRITICAL FIX: NULL means immediate completion - no callback will be invoked
      intrusive_ptr_decrement(this);  // Release reference for immediate completion
    }
    // For non-NULL, non-error requests: callback will be invoked and will handle cleanup
  }
  
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* task = static_cast<ServerReadResponseTask*>(user_data);
    ABSL_LOG(INFO) << "UCX native read response sent: " << ucs_status_string(status);
    
    // CRITICAL FIX: Only free request if it's a valid UCX request pointer
    if (request != nullptr) {
      ucp_request_free(request);
    }
    
    intrusive_ptr_decrement(task);  // Release reference
  }
};

// Server-side task for handling write responses safely with reference counting
struct ServerWriteResponseTask : public internal::AtomicReferenceCount<ServerWriteResponseTask> {
  ucp_ep_h client_endpoint;
  ucp_context_h context;
  uint64_t request_id;
  uint32_t status_code;
  std::unique_ptr<UcxNativeBuffer> send_buffer;
  
  ServerWriteResponseTask(ucp_ep_h ep, ucp_context_h ctx, uint64_t id, uint32_t status)
    : client_endpoint(ep), context(ctx), request_id(id), status_code(status) {}
  
  void Start() {
    // Use UCX native memory allocation and registration
    send_buffer = UcxNativeBuffer::Create(context, sizeof(WriteResponse));
    if (!send_buffer->is_valid()) {
      ABSL_LOG(ERROR) << "Failed to allocate UCX native buffer for write response";
      return;  // Task will be cleaned up when it goes out of scope
    }
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(send_buffer->data());
    response->header.magic_number = MESSAGE_MAGIC_NUMBER;
    response->header.type = MessageType::WRITE_RESPONSE;
    response->header.key_length = 0;
    response->header.value_length = 0;
    response->header.request_id = request_id;
    response->header.checksum = 0;  // Checksums disabled
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
      intrusive_ptr_decrement(this);  // Cleanup reference we added for callback
    } else if (request == nullptr) {
      // CRITICAL FIX: NULL means immediate completion - no callback will be invoked
      // We must release our reference count ourselves
      ABSL_LOG(INFO) << "Write response sent immediately (synchronous completion)";
      intrusive_ptr_decrement(this);  // Release reference for immediate completion
    }
    // For non-NULL, non-error requests: callback will be invoked and will handle cleanup
  }
  
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* task = static_cast<ServerWriteResponseTask*>(user_data);
    ABSL_LOG(INFO) << "Server write response sent: " << ucs_status_string(status);
    
    // CRITICAL FIX: Only free request if it's a valid UCX request pointer
    // NULL requests indicate immediate completion and should not be freed
    if (request != nullptr) {
      ucp_request_free(request);
    }
    
    // Release our reference count - this is always safe
    intrusive_ptr_decrement(task);
  }
};

// RemoteDramStorage Implementation
void RemoteDramStorage::Store(const std::string& key, const absl::Cord& value) {
  absl::MutexLock lock(&mutex_);
  storage_[key] = value;
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
  
  if (status != UCS_OK) {
    std::cout << "❌ UCX: Send failed [ID:" << request_id << "] - " << ucs_status_string(status) << std::endl;
  }
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
  
  // PHASE 1 OPTIMIZATION 4: Direct processing for simple operations
  // This is a simple operation that can be processed directly in UCX context
  UcxManager::Instance().CompletePendingOperationDirect(request_id, result_status);
  
  // UCX native memory management - buffers are automatically cleaned up
  if (from_ucx_pool) {
    // UCX native buffer - cleanup handled by UcxNativeBuffer destructor
    // No manual cleanup needed
  } else {
    // Regular allocated buffer, use delete[]
    delete[] buffer_to_free;
  }
  delete data;
  
  // CRITICAL FIX: Only free request if it's a valid UCX request pointer
  if (request != nullptr) {
    ucp_request_free(request);
  }
}

/// Unified send callback for all UCX send operations
void SendCallbackUnified(void* request, ucs_status_t status, void* user_data) {
  // Handle unified user data with automatic UCX memory cleanup
  struct SendUserDataUnified {
    uint64_t request_id;
    UnifiedBufferManager::ManagedBuffer buffer;  // RAII cleanup
  };

  SendUserDataUnified* data = static_cast<SendUserDataUnified*>(user_data);
  uint64_t request_id = data->request_id;

  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "UCX send failed [ID:" << request_id << "]: "
                    << ucs_status_string(status);
  }

  absl::Status result_status = (status == UCS_OK) ?
    absl::OkStatus() :
    absl::InternalError(absl::StrFormat("UCX send failed: %s",
                                       ucs_status_string(status)));

  // Complete operation using direct path
  UcxManager::Instance().CompletePendingOperationDirect(request_id, result_status);

  // Automatic cleanup: ManagedBuffer destructor handles UCX memory
  delete data;

  // Handle UCX request cleanup
  if (request != nullptr) {
    ucp_request_free(request);
  }
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

// Forward declaration for UCX native server receive callback
void UcxNativeServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data);

/// UCX native receive completion callback for server-side message handling
void UcxNativeServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX native server receive completed with status: " << ucs_status_string(status);
  
  // Extract UCX native user data structure
  struct UcxNativeReceiveUserData {
    std::unique_ptr<UcxNativeBuffer> buffer;
    size_t buffer_size;
  };
  
  UcxNativeReceiveUserData* native_data = static_cast<UcxNativeReceiveUserData*>(user_data);
  char* buffer = native_data->buffer->data();
  
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
      
      // UCX provides transport-level reliability, checksums not needed
      
      // PERFORMANCE OPTIMIZATION: Direct string_view construction eliminates unnecessary allocation
      std::string key(data_ptr, header->key_length);
      
      // FIX: Create a proper Cord that owns its data
      // The UCX buffer will be freed/reused, so we need to copy the data
      std::string value_str(data_ptr + header->key_length, header->value_length);
      absl::Cord value(value_str);
      
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
    
    // Post another adaptive receive buffer immediately to maintain server capacity
    // This must be done synchronously to avoid gaps in receive buffer availability
    auto& ucx_manager = UcxManager::Instance();
    if (ucx_manager.GetContext() && ucx_manager.GetWorker() && !ucx_manager.IsShuttingDown()) {
      ucx_manager.PostAdaptiveServerReceive();
    }
  } else {
    ABSL_LOG(ERROR) << "Failed to receive adaptive message: " << ucs_status_string(status);
  }
  
cleanup_and_exit:
  // Clean up UCX native buffer - automatic cleanup via unique_ptr destructor
  // The UcxNativeBuffer destructor will call ucp_mem_unmap() automatically
  
  // Clean up user data
  delete native_data;
  
  // CONCURRENCY CONTROL: Decrement active receive buffer counter
  UcxManager::Instance().active_receive_buffers_.fetch_sub(1, std::memory_order_acq_rel);
  
  // Remove request from active list and free the UCX request
  if (request != nullptr) {
    UcxManager::Instance().RemoveActiveRequest(request);
    ucp_request_free(request);
  }
}

// Removed ServerReceiveCallback - using UCX native UcxNativeServerReceiveCallback instead

/// Client-side callback for handling read responses
void ClientReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  
  // Extract user data structure
  struct ReadUserData {
    uint64_t request_id;
    char* buffer;
  };
  ReadUserData* read_data = static_cast<ReadUserData*>(user_data);
  uint64_t request_id = read_data->request_id;
  char* buffer = read_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(ReadResponse)) {
    // PERFORMANCE OPTIMIZATION: Eliminate copy #4 - Direct buffer processing
    // Zero-copy access to UCX receive buffer without intermediate copy
    char* response_data_ptr = buffer;
    size_t response_data_size = info->length;
    
    message_utils::LogMessageBuffer(response_data_ptr, response_data_size, "Client received read response");
    
    ReadResponse* response = reinterpret_cast<ReadResponse*>(response_data_ptr);
    
    kvstore::ReadResult result;
    if (response->status_code == 0 && response->header.value_length > 0) {
      // Validate value_length is reasonable
      if (response->header.value_length > 4194304) {  // Set to block size
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
        const char* value_ptr = response_data_ptr + sizeof(ReadResponse);
        
        // PERFORMANCE OPTIMIZATION: Zero-copy Cord construction with proper UCX buffer ownership
        // Transfer ownership of UCX buffer to Cord to eliminate copy #5
        char* owned_buffer = buffer;  // Transfer ownership
        result.value = absl::MakeCordFromExternal(
          absl::string_view(value_ptr, response->header.value_length),
          [owned_buffer](absl::string_view) { 
            // UCX native memory - cleanup handled automatically
            // No manual buffer release needed for UCX native buffers
            delete[] owned_buffer;  // Only for non-UCX buffers
          }
        );
        buffer = nullptr;  // Ownership transferred, prevent double delete
        result.stamp.generation = StorageGeneration::FromString("remote_read");
        result.stamp.time = absl::Now();
        // ABSL_LOG(INFO) << "Read successful, value size=" << result.value.size();
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
    
    // Only release buffer if ownership wasn't transferred to Cord
    if (buffer != nullptr) {
      // UCX native memory - cleanup handled automatically
      delete[] buffer;  // Only for non-UCX buffers
    }
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
    
    // UCX native memory - cleanup handled automatically
    delete[] buffer;  // Only for non-UCX buffers
    delete read_data;
  }
  
  // CRITICAL FIX: Only free request if it's a valid UCX request pointer
  if (request != nullptr) {
    ucp_request_free(request);
  }
}

/// Client-side callback for handling write responses
void ClientWriteResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  
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
    
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received write response");
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(response_data.data());
    
    absl::Status result_status;
    if (response->status_code == 0) {
      // Success
      result_status = absl::OkStatus();
    } else {
      // Error
      result_status = absl::InternalError("Write failed on server");
      ABSL_LOG(INFO) << "Write failed for request_id=" << response->header.request_id;
      std::cout << "❌ CLIENT: Write response [ID:" << response->header.request_id << "] FAILED" << std::endl;
    }
    
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple write response
    UcxManager::Instance().CompletePendingOperationDirect(request_id, result_status);
    
    // UCX native memory - cleanup handled automatically
    delete[] buffer;  // Only for non-UCX buffers
    delete write_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive write response: " << ucs_status_string(status);
    
    // PHASE 1 OPTIMIZATION 4: Direct completion for simple error case
    UcxManager::Instance().CompletePendingOperationDirect(request_id, 
        absl::InternalError(absl::StrFormat("Failed to receive write response: %s", 
                                           ucs_status_string(status))));
    
    // UCX native memory - cleanup handled automatically
    delete[] buffer;  // Only for non-UCX buffers
    delete write_data;
  }
  
  // CRITICAL FIX: Only free request if it's a valid UCX request pointer
  if (request != nullptr) {
    ucp_request_free(request);
  }
}



}  // namespace

// PHASE 1 OPTIMIZATION 1 + 3: Zero-Copy Buffer Management with UCX Integration
class ZeroCopyMessageBuilder {
public:
  struct MessageResult {
    UnifiedBufferManager::ManagedBuffer buffer;
    size_t total_size;
    ucp_mem_h mem_handle;  // For UCX send parameters
  };

  static std::optional<MessageResult> BuildWriteMessage(
      ucp_context_h context,
      const kvstore::Key& key,
      const absl::Cord& value,
      uint64_t request_id) {

    size_t total_size = sizeof(MessageHeader) + key.size() + value.size();

    auto buffer_opt = UnifiedBufferManager::AllocateForSend(context, total_size);
    if (!buffer_opt) {
      ABSL_LOG(ERROR) << "Failed to allocate buffer for write message";
      return std::nullopt;
    }

    auto& buffer = *buffer_opt;

    // Initialize header directly in registered buffer
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.data());
    char* payload = buffer.data() + sizeof(MessageHeader);

    // Direct key copy (no intermediate storage)
    std::memcpy(payload, key.data(), key.size());

    // Direct cord-to-buffer copy (avoid string conversion)
    size_t offset = key.size();
    for (absl::string_view chunk : value.Chunks()) {
      std::memcpy(payload + offset, chunk.data(), chunk.size());
      offset += chunk.size();
    }

    // Initialize header
    message_utils::InitializeMessageHeader(
        header, MessageType::WRITE_REQUEST,
        static_cast<uint32_t>(key.size()),
        static_cast<uint32_t>(value.size()),
        request_id, payload);

    return MessageResult{
        .buffer = std::move(buffer),
        .total_size = total_size,
        .mem_handle = buffer_opt->mem_handle()
    };
  }

  static std::optional<MessageResult> BuildReadMessage(
      ucp_context_h context,
      const kvstore::Key& key,
      uint64_t request_id) {

    size_t total_size = sizeof(MessageHeader) + key.size();

    auto buffer_opt = UnifiedBufferManager::AllocateForSend(context, total_size);
    if (!buffer_opt) {
      ABSL_LOG(ERROR) << "Failed to allocate buffer for read message";
      return std::nullopt;
    }

    auto& buffer = *buffer_opt;

    // Initialize header directly in registered buffer
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer.data());
    char* payload = buffer.data() + sizeof(MessageHeader);

    // Direct key copy (no intermediate storage)
    std::memcpy(payload, key.data(), key.size());

    // Initialize header
    message_utils::InitializeMessageHeader(
        header, MessageType::READ_REQUEST,
        static_cast<uint32_t>(key.size()),
        0, // No value for read request
        request_id, payload);

    return MessageResult{
        .buffer = std::move(buffer),
        .total_size = total_size,
        .mem_handle = buffer_opt->mem_handle()
    };
  }

  // Legacy methods for backward compatibility during transition
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
  return 0;  // Checksums disabled - UCX provides transport-level reliability
}

bool VerifyMessageHeader(const MessageHeader* header, size_t total_message_size) {
  if (!header) {
    ABSL_LOG(ERROR) << "Message header is null";
    return false;
  }
  return true;  // Basic null check only - UCX provides transport reliability
}

void InitializeMessageHeader(MessageHeader* header, MessageType type, 
                            uint32_t key_length, uint32_t value_length, 
                            uint64_t request_id, const void* payload_data) {
  header->magic_number = MESSAGE_MAGIC_NUMBER;
  header->type = type;
  header->key_length = key_length;
  header->value_length = value_length;
  header->request_id = request_id;
  
  header->checksum = 0;  // Checksums disabled
}

void LogMessageBuffer(const char* buffer, size_t size, const std::string& prefix) {
  // Empty implementation - logging disabled for performance
}

}  // namespace message_utils

// Removed AdaptiveBufferPool - now using UCX native memory management
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
  }  // Release mutex here
  
  // UCX native memory management - no buffer pool initialization needed
  
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
  
  // Post initial receive buffers using adaptive buffer management
  // CONCURRENCY LIMIT: Set to 5 for stable operation under concurrent load
  // Testing shows server crashes with 10+ concurrent operations due to UCX bugs
  constexpr int initial_adaptive_buffers = 5;  // Limited for stability
  ABSL_LOG(INFO) << "Posting " << initial_adaptive_buffers << " initial adaptive receive buffers (limited for stability)";
  for (int i = 0; i < initial_adaptive_buffers; ++i) {
    PostAdaptiveServerReceive(true);  // true = mutex already locked
  }
  
  return listener;
}

Result<ucp_ep_h> UcxManager::CreateClientEndpoint(const std::string& server_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
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
  ABSL_LOG(INFO) << "  Socket port: " << ntohs(server_sockaddr.sin_port);
  
  // Create the client endpoint
  ucp_ep_h client_endpoint;
  ucs_status_t status = ucp_ep_create(worker_, &ep_params, &client_endpoint);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create UCX client endpoint: " << ucs_status_string(status);
    return absl::InternalError(absl::StrFormat(
        "Failed to create UCX client endpoint to %s: %s",
        server_addr, ucs_status_string(status)));
  }
  
  ABSL_LOG(INFO) << "UCX client endpoint created successfully to " << server_addr;
  
  // Register this endpoint for cleanup
  RegisterClientSideEndpointNoLock(client_endpoint);
  
  ABSL_LOG(INFO) << "Client endpoint registered, waiting for connection establishment";
  
  // Wait for connection to be established
  // UCX endpoints need time to establish connection before they can send data
  // We'll poll the endpoint status until it's connected or timeout
  constexpr int kMaxConnectionAttempts = 50;  // 5 seconds with 100ms intervals
  bool connection_established = false;
  
  for (int attempt = 0; attempt < kMaxConnectionAttempts; ++attempt) {
    // Release mutex temporarily to allow progress thread to work
    mutex_.Unlock();
    
    // Allow UCX progress thread to process connection establishment
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Re-acquire mutex
    mutex_.Lock();
    
    // Check if we're still initialized (in case of shutdown during wait)
    if (!initialized_) {
      return absl::InternalError("UCX manager shutdown during connection establishment");
    }
    
    // Test the connection by checking if we can get endpoint info
    // If the endpoint is connected, this should succeed
    ucp_ep_attr_t ep_attr;
    ep_attr.field_mask = UCP_EP_ATTR_FIELD_REMOTE_SOCKADDR;
    ucs_status_t attr_status = ucp_ep_query(client_endpoint, &ep_attr);
    
    if (attr_status == UCS_OK) {
      connection_established = true;
      ABSL_LOG(INFO) << "UCX client endpoint connection established after " << (attempt + 1) << " attempts";
      break;
    }
    
    if (attempt % 10 == 0) {  // Log every 1 second
      ABSL_LOG(INFO) << "Waiting for UCX endpoint connection... attempt " << (attempt + 1) << "/" << kMaxConnectionAttempts;
    }
  }
  
  if (!connection_established) {
    ABSL_LOG(ERROR) << "UCX client endpoint failed to establish connection after " << kMaxConnectionAttempts << " attempts";
    return absl::InternalError(absl::StrFormat(
        "Failed to establish UCX connection to %s: connection timeout", server_addr));
  }
  
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

// Removed simple CompletePendingOperation - using optimized CompletePendingOperationDirect

// Removed simple CompletePendingReadOperation - using optimized CompletePendingReadOperationDirect

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
    CompletePendingOperationDirect(request_id, status);
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
    CompletePendingReadOperationDirect(request_id, std::move(result));
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


void UcxManager::PostAdaptiveServerReceive(bool already_locked) {
  // CONCURRENCY CONTROL: Check if we've reached the limit
  int current_buffers = active_receive_buffers_.load(std::memory_order_acquire);
  if (current_buffers >= kMaxConcurrentReceiveBuffers) {
    ABSL_LOG(INFO) << "Skipping PostAdaptiveServerReceive: at limit (" 
                   << current_buffers << "/" << kMaxConcurrentReceiveBuffers << ")";
    return;
  }
  
  // Use UCX native memory for receive buffer
  size_t buffer_size = sizeof(MessageHeader) + 256 + 4194304;  // 4MB + header
  auto native_buffer = UcxNativeBuffer::Create(context_, buffer_size);
  
  if (!native_buffer || !native_buffer->is_valid()) {
    ABSL_LOG(ERROR) << "Failed to allocate UCX native receive buffer";
    return;
  }
  
  // Create user data structure to track UCX native buffer
  struct UcxNativeReceiveUserData {
    std::unique_ptr<UcxNativeBuffer> buffer;
    size_t buffer_size;
  };
  
  UcxNativeReceiveUserData* user_data = new UcxNativeReceiveUserData{
    std::move(native_buffer),
    buffer_size
  };
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = UcxNativeServerReceiveCallback;
  recv_params.user_data = user_data;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
  // Get worker handle with proper locking
  ucp_worker_h worker_handle;
  if (already_locked) {
    worker_handle = worker_;
  } else {
    absl::MutexLock lock(&mutex_);
    worker_handle = worker_;
  }
  
  if (!worker_handle) {
    ABSL_LOG(ERROR) << "Cannot post UCX native server receive: worker is null";
    delete user_data;
    return;
  }
  
  // Increment active receive buffer counter
  active_receive_buffers_.fetch_add(1, std::memory_order_acq_rel);
  
  void* request = ucp_tag_recv_nbx(worker_handle, user_data->buffer->data(), buffer_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post UCX native server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    delete user_data;
    // Decrement counter on failure
    active_receive_buffers_.fetch_sub(1, std::memory_order_acq_rel);
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    // Track the request for cleanup
    if (already_locked) {
      active_requests_.push_back(request);
    } else {
      absl::MutexLock lock(&mutex_);
      active_requests_.push_back(request);
    }
  } else {
    // Receive completed immediately (unlikely)
    delete user_data;
    // Decrement counter since receive completed immediately
    active_receive_buffers_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void UcxManager::CancelPendingReceives() {
  absl::MutexLock lock(&mutex_);
  CancelPendingReceivesNoLock();
}

void UcxManager::CancelPendingReceivesNoLock() {
  // This method assumes the mutex is already held
  ABSL_LOG(INFO) << "Canceling " << active_requests_.size() << " pending UCX requests";
  
  if (active_requests_.empty()) {
    return;
  }
  
  // Cancel all active requests
  for (void* request : active_requests_) {
    if (request != nullptr) {
      ucp_request_cancel(worker_, request);
    }
  }
  
  // Wait for cancellation to complete by processing worker progress
  // The callbacks will free the requests when cancellation completes
  size_t max_iterations = 1000;  // Prevent infinite loop
  size_t iteration = 0;
  
  while (!active_requests_.empty() && iteration < max_iterations) {
    // Release mutex temporarily to allow callbacks to execute
    mutex_.Unlock();
    
    // Process UCX events to allow cancellation callbacks to run
    if (worker_) {
      ucp_worker_progress(worker_);
    }
    
    // Small delay to allow callbacks to complete
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    
    mutex_.Lock();
    iteration++;
  }
  
  if (!active_requests_.empty()) {
    ABSL_LOG(WARNING) << "Still have " << active_requests_.size() 
                      << " pending requests after cancellation timeout";
  }
  
  // Force clear any remaining requests to prevent memory leaks
  active_requests_.clear();
}

void UcxManager::RemoveActiveRequest(void* request) {
  absl::MutexLock lock(&mutex_);
  RemoveActiveRequestNoLock(request);
}

void UcxManager::RemoveActiveRequestNoLock(void* request) {
  // This method assumes the mutex is already held
  auto it = std::find(active_requests_.begin(), active_requests_.end(), request);
  if (it != active_requests_.end()) {
    active_requests_.erase(it);
  }
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
      client_endpoint, context_, request_id, value);
  
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
      client_endpoint, context_, request_id, status_code);
  
  ABSL_LOG(INFO) << "Server sending write response: status_code=" << status_code;
  
  // Start the task - it will handle the entire send operation safely
  task->Start();
}

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_.load()) {
    ABSL_LOG(INFO) << "Worker progress task already running";
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
  
}

void UcxManager::WorkerProgressTask() {
  while (true) {
    ucp_worker_h worker_handle = nullptr;
    bool should_continue = false;

    // Get worker handle safely
    {
      absl::MutexLock lock(&mutex_);
      if (!initialized_ || !progress_task_running_) {
        break;
      }
      worker_handle = worker_;
      should_continue = true;
    }

    if (!worker_handle || !should_continue) {
      break;
    }

    // Process all immediately available work
    unsigned progress_count = 0;
    do {
      progress_count = ucp_worker_progress(worker_handle);
      progress_metrics_.RecordProgressCall(progress_count);
    } while (progress_count > 0);

    // Check shutdown condition before potentially blocking
    {
      absl::MutexLock lock(&mutex_);
      if (!progress_task_running_) {
        break;
      }
    }

    progress_metrics_.RecordWaitCycle();

    // Use UCX native wait if enabled, otherwise fallback to sleep
    if (use_native_wait_.load()) {
      // Use UCX native wait mechanism for efficiency
      ucs_status_t arm_status = ucp_worker_arm(worker_handle);
      if (arm_status == UCS_OK) {
        // Worker is armed for notifications, wait for events
        ucs_status_t wait_status = ucp_worker_wait(worker_handle);

        if (wait_status != UCS_OK && wait_status != UCS_ERR_TIMED_OUT) {
          ABSL_LOG(WARNING) << "UCX worker wait failed: " << ucs_status_string(wait_status);
          // Brief fallback sleep on wait failure
          progress_metrics_.RecordFallbackSleep();
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      } else if (arm_status == UCS_ERR_BUSY) {
        // Worker has pending events, continue immediately without waiting
        continue;
      } else {
        ABSL_LOG(WARNING) << "UCX worker arm failed: " << ucs_status_string(arm_status);
        // Fallback to brief sleep on arm failure
        progress_metrics_.RecordFallbackSleep();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    } else {
      // Fallback mode: use the original sleep-based approach
      progress_metrics_.RecordFallbackSleep();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

  ABSL_LOG(INFO) << "UCX worker progress stopped";

  // Log final performance metrics
  progress_metrics_.LogPerformance();
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
  
  // Give UCX some time to process cancellations through the progress thread
  // The progress thread will handle any remaining events
  ABSL_LOG(INFO) << "Allowing progress thread to process remaining UCX events";
  
  // CRITICAL: Clean up the adaptive buffer pool BEFORE setting shutdown flag
  // This ensures BufferInfo destructors can properly unmap UCX memory regions
  // UCX native memory management - no buffer pool shutdown needed
  
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

    std::cout << "\n####################################\nCALLING WRITE REMOTE for key: " << key << "\n####################################\n"<<std::endl;
    // Reinitialize client endpoint if null (connection persistence fix)
    if (!client_endpoint_) {
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
    
    // Use unified buffer management for message construction
    auto message_result_opt = ZeroCopyMessageBuilder::BuildWriteMessage(
        ucx_manager.GetContext(), key, value, request_id);

    if (!message_result_opt) {
      return absl::InternalError("Failed to allocate message buffer");
    }

    auto& message_result = *message_result_opt;

    // Log message for debugging
    message_utils::LogMessageBuffer(message_result.buffer.data(),
                                  message_result.total_size,
                                  "Client sending unified write request");
    
    ABSL_LOG(INFO) << "Sending UCX write request: key='" << key << "', value_size=" << value.size() << ", request_id=" << request_id;
    std::cout << "📤 CLIENT: Sending write request [ID:" << request_id << "]" << std::endl;
    
    // Create promise/future pair
    auto [promise, future] = PromiseFuturePair<void>::Make();
    
    // Register pending operation
    ucx_manager.RegisterPendingOperation(request_id, std::move(promise), MessageType::WRITE_REQUEST);
    
    // Post write response receive with unified buffer
    PostWriteResponseReceive(request_id);

    // Create user data for callback - now holds ManagedBuffer
    struct SendUserDataUnified {
      uint64_t request_id;
      UnifiedBufferManager::ManagedBuffer buffer;  // RAII cleanup
    };

    auto* user_data = new SendUserDataUnified{
        .request_id = request_id,
        .buffer = std::move(message_result.buffer)
    };

    // Send message using UCX tagged messaging with registered memory
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA |
                              UCP_OP_ATTR_FIELD_MEMH;  // Use registered memory
    send_params.cb.send = SendCallbackUnified;
    send_params.memh = user_data->buffer.mem_handle();  // Get handle from moved buffer
    send_params.user_data = user_data;

    void* request = ucp_tag_send_nbx(client_endpoint_, user_data->buffer.data(),
                                     message_result.total_size,
                                     UCX_TAG_WRITE_REQUEST, &send_params);
    
    // Handle request status with unified cleanup
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX send failed: " << ucs_status_string(error_status);
      ucx_manager.CompletePendingOperationDirect(request_id,
          absl::InternalError("UCX send failed"));
      delete user_data;  // ManagedBuffer automatically cleans up UCX memory
    } else if (request == nullptr) {
      // Immediate completion
      delete user_data;  // ManagedBuffer automatically cleans up UCX memory
    }
    // For non-NULL requests: SendCallbackUnified will handle cleanup
    
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
    
    // PHASE 1 OPTIMIZATION 1: Zero-copy read message construction
    size_t message_size;
    auto message_buffer = ZeroCopyMessageBuilder::BuildReadMessage(
        key, request_id, &message_size);
    
    // Log message buffer for debugging
    message_utils::LogMessageBuffer(message_buffer.get(), message_size, "Client sending read request");
    
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
      ucx_manager.CompletePendingReadOperationDirect(request_id, std::move(error_result));
      // Clean up resources - ReadRemote uses regular allocation
      delete[] stable_buffer;
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately - response handling will complete the read
      // Clean up resources - ReadRemote uses regular allocation
      delete[] stable_buffer;
      delete user_data;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      // Resources will be cleaned up in SendCallback
    }
    
    return future;
  }
  
  /// Post a receive buffer to get read response from server
  void PostReadResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the read response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response using regular allocation (simpler management)
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
      ucx_manager.CompletePendingReadOperationDirect(request_id, std::move(error_result));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
    } else {
      // Receive completed immediately (unlikely for read responses)
      delete[] recv_buffer;
      delete user_data;
    }
  }
  
  /// Post a receive buffer to get write response from server
  void PostWriteResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the write response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response using regular allocation
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
      ucx_manager.CompletePendingOperationDirect(request_id, 
          absl::InternalError("Failed to post write response receive"));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
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
    auto endpoint_result = ucx_manager.CreateClientEndpoint(*data_.remote_addr);
    if (!endpoint_result.ok()) {
      return endpoint_result.status();
    }
    
    driver->client_endpoint_ = *endpoint_result;
    driver->is_server_mode_ = false;
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
 