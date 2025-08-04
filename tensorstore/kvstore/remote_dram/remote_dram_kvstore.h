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

#ifndef TENSORSTORE_KVSTORE_REMOTE_DRAM_REMOTE_DRAM_KVSTORE_H_
#define TENSORSTORE_KVSTORE_REMOTE_DRAM_REMOTE_DRAM_KVSTORE_H_

/// \file
/// Remote DRAM key-value store backed by UCX for direct memory-to-memory transfer.

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

#include "tensorstore/internal/json_binding/json_binding.h"
#include "tensorstore/kvstore/driver.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/read_result.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/synchronization/mutex.h"

// UCX headers
#include <ucp/api/ucp.h>

namespace tensorstore {

namespace jb = tensorstore::internal_json_binding;

// Constants for message integrity and UCX tags
constexpr uint32_t MESSAGE_MAGIC_NUMBER = 0xDEADBEEF;
constexpr ucp_tag_t UCX_TAG_WRITE_REQUEST = 0x1000;
constexpr ucp_tag_t UCX_TAG_WRITE_RESPONSE = 0x1001;
constexpr ucp_tag_t UCX_TAG_READ_REQUEST = 0x2000;
constexpr ucp_tag_t UCX_TAG_READ_RESPONSE = 0x2001;
constexpr ucp_tag_t UCX_TAG_MASK = 0xF000;

/// Message types for client-server communication
enum class MessageType : uint32_t {
  WRITE_REQUEST = 1,
  WRITE_RESPONSE = 2,
  READ_REQUEST = 3,
  READ_RESPONSE = 4,
};

/// Header for all messages
struct MessageHeader {
  uint32_t magic_number;   // Magic number for integrity verification
  MessageType type;
  uint32_t key_length;
  uint32_t value_length;
  uint64_t request_id;
  uint32_t checksum;       // Simple checksum for data integrity
} __attribute__((packed));

/// Write response message structure  
struct WriteResponse {
  MessageHeader header;
  uint32_t status_code;  // 0 = success, non-zero = error
} __attribute__((packed));

/// Read response message structure
struct ReadResponse {
  MessageHeader header;
  uint32_t status_code;  // 0 = success, 1 = key not found, 2 = error
  // Followed by value_length bytes of value data (if status_code == 0)
} __attribute__((packed));

/// Data members for `RemoteDramDriverSpec`.
struct RemoteDramDriverSpecData {
  /// Server listen address (for server mode)
  std::optional<std::string> listen_addr;
  
  /// Remote server address (for client mode)
  std::optional<std::string> remote_addr;

  /// Make this type compatible with `tensorstore::ApplyMembers`.
  constexpr static auto ApplyMembers = [](auto&& x, auto f) {
    return f(x.listen_addr, x.remote_addr);
  };

  /// JSON binding for the spec data
  constexpr static auto default_json_binder = jb::Object(
      jb::Member("listen_addr", 
                 jb::Projection<&RemoteDramDriverSpecData::listen_addr>()),
      jb::Member("remote_addr", 
                 jb::Projection<&RemoteDramDriverSpecData::remote_addr>())
  );
};

/// Context for pending write operations
struct PendingWriteOperation {
  uint64_t request_id;
  Promise<void> promise;
  
  explicit PendingWriteOperation(uint64_t id, Promise<void> p) 
    : request_id(id), promise(std::move(p)) {}
};

/// Context for pending read operations
struct PendingReadOperation {
  uint64_t request_id;
  Promise<kvstore::ReadResult> promise;
  
  explicit PendingReadOperation(uint64_t id, Promise<kvstore::ReadResult> p)
    : request_id(id), promise(std::move(p)) {}
};

/// High-performance async operation manager for scalable concurrency
class AsyncOperationManager {
public:
  static constexpr size_t kMaxConcurrentReads = 32;   // Limit concurrent reads
  static constexpr size_t kMaxConcurrentWrites = 64;  // Higher limit for writes
  
  struct Operation {
    enum Type { READ, WRITE };
    Type type;
    uint64_t request_id;
    std::chrono::steady_clock::time_point start_time;
  };
  
  void RegisterOperation(Operation::Type type, uint64_t request_id) {
    absl::MutexLock lock(&mutex_);
    Operation op{type, request_id, std::chrono::steady_clock::now()};
    
    if (type == Operation::READ) {
      pending_reads_.emplace(request_id, op);
    } else {
      pending_writes_.emplace(request_id, op);
    }
  }
  
  void CompleteOperation(Operation::Type type, uint64_t request_id) {
    absl::MutexLock lock(&mutex_);
    if (type == Operation::READ) {
      pending_reads_.erase(request_id);
    } else {
      pending_writes_.erase(request_id);
    }
  }
  
  bool ShouldThrottleReads() const {
    absl::MutexLock lock(&mutex_);
    return pending_reads_.size() >= kMaxConcurrentReads;
  }
  
  size_t GetPendingReadCount() const {
    absl::MutexLock lock(&mutex_);
    return pending_reads_.size();
  }
  
private:
  mutable absl::Mutex mutex_;
  std::unordered_map<uint64_t, Operation> pending_reads_ ABSL_GUARDED_BY(mutex_);
  std::unordered_map<uint64_t, Operation> pending_writes_ ABSL_GUARDED_BY(mutex_);
};

/// Adaptive buffer management for UCX receive operations
class AdaptiveBufferPool {
 public:
  /// Buffer size categories optimized for large chunk transfers
  static constexpr size_t kSmallBufferSize = 16384;      // 1KB for headers/small messages
  static constexpr size_t kMediumBufferSize = 65536;    // 64KB for payloads up to 32KB + header overhead
  static constexpr size_t kLargeBufferSize = 4194304;   // 4MB for large transfers (up to 4MB + header overhead)
  static constexpr size_t kMaxBuffers = 200;            // Reduced max buffers but larger sizes
  static constexpr size_t kInitialBuffers = 10;         // Fewer initial buffers to save memory
  
  /// Phase 1: Safe buffer tiers with exact current defaults
  struct BufferTier {
    size_t size;
    size_t initial_count;
    size_t max_count;
    size_t reserved;  // For future use
  };
  
  static constexpr BufferTier kSafeTiers[] = {
    {1024,   20,  100, 0},   // Current small (unchanged)
    {40960,  20,  100, 0},   // Updated medium for 32KB+ support
    {69632,  20,  100, 0},   // Current large (unchanged)
    {262144, 0,   0,   0},   // New tier (disabled by default)
    {1048576, 0,  0,   0},   // New tier (disabled by default)
  };
  
  struct BufferInfo {
    char* data;
    size_t size;
    std::chrono::steady_clock::time_point allocated_time;
    bool in_use;
    // PHASE 1 OPTIMIZATION 3: UCX memory registration support
    ucp_mem_h memory_handle;
    bool is_ucx_registered;
    
    BufferInfo(size_t buffer_size) 
      : data(new char[buffer_size]), size(buffer_size), 
        allocated_time(std::chrono::steady_clock::now()), in_use(false),
        memory_handle(nullptr), is_ucx_registered(false) {}
    
    ~BufferInfo();
  };
  
  /// Initialize buffer pool with initial allocations
  void Initialize();
  
  /// Get an appropriate buffer for the given size requirement
  char* AcquireBuffer(size_t required_size, size_t* actual_size);
  
  /// PHASE 1 OPTIMIZATION 3: UCX-aware buffer acquisition with registration
  char* AcquireUcxBuffer(size_t required_size, size_t* actual_size, ucp_mem_h* mem_handle);
  
  /// Return a buffer to the pool for reuse
  void ReleaseBuffer(char* buffer);
  
  /// Clean up unused buffers older than threshold
  void CleanupOldBuffers(std::chrono::seconds max_age = std::chrono::seconds(30));
  
  /// Get buffer pool statistics
  struct Stats {
    size_t total_buffers;
    size_t buffers_in_use;
    size_t small_buffers;
    size_t medium_buffers;
    size_t large_buffers;
    size_t memory_usage_bytes;
  };
  Stats GetStats() const;
  
  /// Shutdown and cleanup all buffers
  void Shutdown();
  
 private:
  mutable absl::Mutex mutex_;
  std::vector<std::unique_ptr<BufferInfo>> buffer_pool_ ABSL_GUARDED_BY(mutex_);
  std::unordered_map<char*, size_t> buffer_lookup_ ABSL_GUARDED_BY(mutex_);
  bool initialized_ ABSL_GUARDED_BY(mutex_) = false;
  
  /// Find best fit buffer size
  size_t SelectBufferSize(size_t required_size) const;
  
  /// Create new buffer of specified size
  std::unique_ptr<BufferInfo> CreateBuffer(size_t size);
  
  /// PHASE 1 OPTIMIZATION 3: Create and register buffer with UCX
  std::unique_ptr<BufferInfo> CreateAndRegisterUcxBuffer(size_t size);
  
  /// Try to register an existing buffer with UCX
  bool TryRegisterBufferWithUcx(BufferInfo* buffer);
};

/// Server-side storage for key-value pairs
class RemoteDramStorage {
 public:
  /// Store a key-value pair
  void Store(const std::string& key, const absl::Cord& value);
  
  /// Retrieve a value by key
  std::optional<absl::Cord> Get(const std::string& key) const;
  
  /// Check if key exists
  bool Exists(const std::string& key) const;
  
  /// Remove a key
  bool Remove(const std::string& key);
  
  /// Get all stored keys (for verification/debugging)
  std::vector<std::string> GetAllKeys() const;
  
  /// Get storage statistics
  size_t GetKeyCount() const;
  
 private:
  mutable absl::Mutex mutex_;
  std::unordered_map<std::string, absl::Cord> storage_ ABSL_GUARDED_BY(mutex_);
  
};

/// Singleton UCX Manager to handle global UCX state and worker polling
class UcxManager {
 public:
  /// Get the singleton instance
  static UcxManager& Instance();
  
  /// Initialize UCX context and worker
  absl::Status Initialize();
  
  /// Get the UCX context
  ucp_context_h GetContext() const { return context_; }
  
  /// Get the UCX worker
  ucp_worker_h GetWorker() const { return worker_; }
  
  /// Create a UCX listener for server mode
  Result<ucp_listener_h> CreateListener(const std::string& listen_addr);
  
  /// Create a client endpoint to connect to a server
  Result<ucp_ep_h> CreateClientEndpoint(const std::string& server_addr);
  
  /// Get the server storage (for server mode)
  RemoteDramStorage& GetStorage() { return storage_; }
  
  /// Register a pending write operation
  void RegisterPendingOperation(uint64_t request_id, Promise<void> promise, MessageType type);
  
  /// Register a pending read operation
  void RegisterPendingReadOperation(uint64_t request_id, Promise<kvstore::ReadResult> promise);
  
  /// Complete a pending write operation
  void CompletePendingOperation(uint64_t request_id, absl::Status status);
  
  /// Complete a pending read operation
  void CompletePendingReadOperation(uint64_t request_id, kvstore::ReadResult result);
  
  /// PHASE 1 OPTIMIZATION 4: Fast-path completion for simple operations (lock-free when possible)
  void CompletePendingOperationDirect(uint64_t request_id, absl::Status status);
  
  /// PHASE 1 OPTIMIZATION 4: Fast-path read completion for simple operations (lock-free when possible) 
  void CompletePendingReadOperationDirect(uint64_t request_id, kvstore::ReadResult result);
  
  /// Generate next request ID
  uint64_t GenerateRequestId();
  
  /// Post a receive buffer for server-side message handling
  void PostServerReceive();
  
  /// Post receive buffer without acquiring mutex (for use within locked context)
  void PostServerReceiveNoLock();
  
  /// Post adaptive receive buffer with optimal sizing
  void PostAdaptiveServerReceive();
  
  /// Post adaptive receive buffer without acquiring mutex (for use within locked context)
  void PostAdaptiveServerReceiveNoLock();
  
  /// Get the adaptive buffer pool
  AdaptiveBufferPool& GetBufferPool() { return buffer_pool_; }
  
  /// Get the async operation manager for throttling
  AsyncOperationManager& GetAsyncManager() { return async_manager_; }
  
  /// Send a read response from server to client
  void SendReadResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                        const std::optional<absl::Cord>& value);
  
  /// Send a write response from server to client
  void SendWriteResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                         uint32_t status_code);
  
  /// Register a client endpoint (for server mode)
  void RegisterClientEndpoint(ucp_ep_h client_endpoint);
  
  /// Register a server endpoint (for client mode)
  void RegisterServerEndpoint(ucp_ep_h server_endpoint);
  
  /// Register a client-side endpoint (for client mode cleanup)
  void RegisterClientSideEndpoint(ucp_ep_h client_endpoint);
  
  /// Register a client-side endpoint (for client mode cleanup) - assumes mutex is held
  void RegisterClientSideEndpointNoLock(ucp_ep_h client_endpoint);
  
  /// Get a client endpoint for sending responses (for server mode)
  /// Returns the most recently connected client endpoint
  ucp_ep_h GetClientEndpoint() const;
  
  /// Cancel all pending receive operations
  void CancelPendingReceives();
  
  /// Cancel all pending receive operations (assumes mutex is held)
  void CancelPendingReceivesNoLock();
  
  /// Cleanup UCX listener if it exists
  void CleanupListener();
  
  /// Cleanup UCX listener if it exists (assumes mutex is held)
  void CleanupListenerNoLock();
  
  /// Shutdown UCX resources
  void Shutdown();
  
 private:
  UcxManager() = default;
  ~UcxManager();
  
  /// Start the worker progress polling task
  void StartWorkerProgressTask();
  
  /// Worker progress polling function
  void WorkerProgressTask();
  
  mutable absl::Mutex mutex_;
  bool initialized_ ABSL_GUARDED_BY(mutex_) = false;
  ucp_context_h context_ ABSL_GUARDED_BY(mutex_) = nullptr;
  ucp_worker_h worker_ ABSL_GUARDED_BY(mutex_) = nullptr;
  ucp_listener_h listener_ ABSL_GUARDED_BY(mutex_) = nullptr;
  
  // ENHANCED: Use atomic bool for thread-safe progress task state
  std::atomic<bool> progress_task_running_{false};
  // ENHANCED: Use managed thread for proper cleanup
  std::unique_ptr<std::thread> progress_thread_ ABSL_GUARDED_BY(mutex_);
  
  /// Server-side storage
  RemoteDramStorage storage_;
  
  /// Adaptive buffer pool for efficient memory management
  AdaptiveBufferPool buffer_pool_;
  
  /// Async operation manager for throttling concurrent operations
  AsyncOperationManager async_manager_;
  
  /// Pending write operations for client mode
  std::unordered_map<uint64_t, std::unique_ptr<PendingWriteOperation>> pending_write_operations_ ABSL_GUARDED_BY(mutex_);
  
  /// Pending read operations for client mode
  std::unordered_map<uint64_t, std::unique_ptr<PendingReadOperation>> pending_read_operations_ ABSL_GUARDED_BY(mutex_);
  
  /// Track active UCX requests for proper cleanup
  std::vector<void*> active_requests_ ABSL_GUARDED_BY(mutex_);
  
  /// Client endpoints for server mode (to send responses back to clients)
  std::vector<ucp_ep_h> client_endpoints_ ABSL_GUARDED_BY(mutex_);
  
  /// Server endpoints for accepting client connections
  std::vector<ucp_ep_h> server_endpoints_ ABSL_GUARDED_BY(mutex_);
  
  /// Client-side endpoints for client mode (for cleanup)
  std::vector<ucp_ep_h> client_side_endpoints_ ABSL_GUARDED_BY(mutex_);
  
  // PHASE 1 OPTIMIZATION 2: Lock-free atomic request ID generation
  std::atomic<uint64_t> next_request_id_{1};
};


/// Client-side callback for handling write responses
void ClientWriteResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data);

/// Utility functions for message integrity
namespace message_utils {
  /// Calculate simple checksum for data integrity
  uint32_t CalculateChecksum(const void* data, size_t size);
  
  /// Verify message header integrity
  bool VerifyMessageHeader(const MessageHeader* header, size_t total_message_size);
  
  /// Initialize message header with integrity fields
  void InitializeMessageHeader(MessageHeader* header, MessageType type, 
                              uint32_t key_length, uint32_t value_length, 
                              uint64_t request_id, const void* payload_data);
  
  /// Log message buffer in hex format for debugging
  void LogMessageBuffer(const char* buffer, size_t size, const std::string& prefix);
}


}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REMOTE_DRAM_REMOTE_DRAM_KVSTORE_H_ 