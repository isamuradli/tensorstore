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

/// UCX-native memory buffer with proper registration for zero-copy operations
class UcxNativeBuffer {
 public:
  static std::unique_ptr<UcxNativeBuffer> Create(ucp_context_h context, size_t size);

  explicit UcxNativeBuffer(ucp_context_h context, size_t size);
  ~UcxNativeBuffer();

  // Non-copyable, movable
  UcxNativeBuffer(const UcxNativeBuffer&) = delete;
  UcxNativeBuffer& operator=(const UcxNativeBuffer&) = delete;
  UcxNativeBuffer(UcxNativeBuffer&&) noexcept;
  UcxNativeBuffer& operator=(UcxNativeBuffer&&) noexcept;

  char* data() { return data_; }
  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool is_valid() const { return mem_handle_ != nullptr && data_ != nullptr; }
  ucp_mem_h mem_handle() const { return mem_handle_; }

 private:
  ucp_context_h context_;
  size_t size_;
  char* data_;
  ucp_mem_h mem_handle_;
};

/// Performance metrics for UCX worker progress thread
class WorkerProgressMetrics {
public:
  struct Stats {
    std::atomic<uint64_t> progress_calls{0};
    std::atomic<uint64_t> events_processed{0};
    std::atomic<uint64_t> wait_cycles{0};
    std::atomic<uint64_t> fallback_sleeps{0};
    std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};
  } stats;

  void RecordProgressCall(unsigned events) {
    stats.progress_calls++;
    stats.events_processed += events;
  }

  void RecordWaitCycle() {
    stats.wait_cycles++;
  }

  void RecordFallbackSleep() {
    stats.fallback_sleeps++;
  }

  void LogPerformance() const {
    auto elapsed = std::chrono::steady_clock::now() - stats.start_time;
    auto seconds = std::chrono::duration<double>(elapsed).count();

    ABSL_LOG(INFO) << "Worker Progress Performance:";
    ABSL_LOG(INFO) << "  Progress calls: " << stats.progress_calls;
    ABSL_LOG(INFO) << "  Events processed: " << stats.events_processed;
    ABSL_LOG(INFO) << "  Wait cycles: " << stats.wait_cycles;
    ABSL_LOG(INFO) << "  Fallback sleeps: " << stats.fallback_sleeps << " ("
                   << (stats.wait_cycles > 0 ? (100.0 * stats.fallback_sleeps / stats.wait_cycles) : 0.0) << "%)";
    ABSL_LOG(INFO) << "  Events/second: " << (seconds > 0 ? (stats.events_processed / seconds) : 0.0);
  }

  // Get efficiency percentage (higher is better)
  double GetEfficiencyPercentage() const {
    if (stats.wait_cycles == 0) return 100.0;
    return 100.0 * (1.0 - static_cast<double>(stats.fallback_sleeps) / stats.wait_cycles);
  }

  // Reset all metrics
  void Reset() {
    stats.progress_calls = 0;
    stats.events_processed = 0;
    stats.wait_cycles = 0;
    stats.fallback_sleeps = 0;
    stats.start_time = std::chrono::steady_clock::now();
  }
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
  
  /// Complete a pending write operation (optimized with fast-path)
  void CompletePendingOperationDirect(uint64_t request_id, absl::Status status);
  
  /// Complete a pending read operation (optimized with fast-path)
  void CompletePendingReadOperationDirect(uint64_t request_id, kvstore::ReadResult result);
  
  /// Generate next request ID
  uint64_t GenerateRequestId();
  
  /// Post UCX native receive buffer with optimal sizing
  /// @param already_locked If true, assumes mutex is already held
  void PostAdaptiveServerReceive(bool already_locked = false);
  
  /// UCX native memory management - no buffer pool needed
  
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
  
  /// Check if UCX is shutting down (safe to call from destructors)
  bool IsShuttingDown() const { return shutdown_started_; }
  
  /// Shutdown UCX resources
  void Shutdown();
  
  /// Remove completed request from active list (for callback use)
  void RemoveActiveRequest(void* request);
  
  /// Remove completed request from active list without acquiring mutex (for use within locked context)
  void RemoveActiveRequestNoLock(void* request);

  /// Get performance metrics for the worker progress thread
  const WorkerProgressMetrics& GetProgressMetrics() const {
    return progress_metrics_;
  }

  /// Enable/disable UCX native wait (for testing and fallback)
  void SetUseNativeWait(bool enable) {
    use_native_wait_ = enable;
  }

  bool IsUsingNativeWait() const {
    return use_native_wait_;
  }

  // CONCURRENCY CONTROL: Public for callback access
  // Atomic counter for active receive buffers
  std::atomic<int> active_receive_buffers_{0};
  
 private:
  UcxManager() = default;
  ~UcxManager();
  
  /// Start the worker progress polling task
  void StartWorkerProgressTask();
  
  /// Worker progress polling function
  void WorkerProgressTask();
  
  mutable absl::Mutex mutex_;
  bool initialized_ ABSL_GUARDED_BY(mutex_) = false;
  std::atomic<bool> shutdown_started_{false};  // Thread-safe flag for shutdown detection
  ucp_context_h context_ ABSL_GUARDED_BY(mutex_) = nullptr;
  ucp_worker_h worker_ ABSL_GUARDED_BY(mutex_) = nullptr;
  ucp_listener_h listener_ ABSL_GUARDED_BY(mutex_) = nullptr;
  
  // ENHANCED: Use atomic bool for thread-safe progress task state
  std::atomic<bool> progress_task_running_{false};
  // ENHANCED: Use managed thread for proper cleanup
  std::unique_ptr<std::thread> progress_thread_ ABSL_GUARDED_BY(mutex_);
  
  /// Server-side storage
  RemoteDramStorage storage_;
  
  /// UCX native memory management - no buffer pool needed
  
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

  // PHASE 1 IMPROVEMENT: Performance metrics and configuration
  WorkerProgressMetrics progress_metrics_;
  std::atomic<bool> use_native_wait_{true};  // Enable UCX native wait by default

  // CONCURRENCY CONTROL: Limit concurrent receive buffers to prevent crashes
  static constexpr int kMaxConcurrentReceiveBuffers = 5;  // Stable limit from testing
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