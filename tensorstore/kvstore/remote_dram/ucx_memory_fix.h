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

#ifndef TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_FIX_H_
#define TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_FIX_H_

/// \file
/// UCX Memory Management Fix - Enhanced remote DRAM kvstore with proper async patterns
/// 
/// This file contains the improved implementation that fixes the critical UCX memory
/// management issues by adopting TensorStore's standard async patterns and UCX best practices.

#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <chrono>

#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/rate_limiter/rate_limiter.h"
#include "tensorstore/kvstore/driver.h"
#include "tensorstore/kvstore/read_result.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/execution/execution.h"
#include "tensorstore/util/executor.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/container/flat_hash_map.h"

// UCX headers
#include <ucp/api/ucp.h>

#include "remote_dram_kvstore.h"  // For existing constants and structures

namespace tensorstore {
namespace remote_dram_enhanced {

using internal::AtomicReferenceCount;
using internal::IntrusivePtr;
using internal::MakeIntrusivePtr;
using internal::RateLimiterNode;
using internal::RateLimiter;

// Forward declarations
class EnhancedRemoteDramDriver;
struct UcxReadTask;
struct UcxWriteTask;
class UcxRequestRegistry;
class UcxBuffer;

/// Enhanced UCX Buffer wrapper with proper RAII
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

/// Enhanced UCX Manager with executor-based threading
class EnhancedUcxManager {
 public:
  static EnhancedUcxManager& Instance() {
    static EnhancedUcxManager instance;
    return instance;
  }
  
  absl::Status Initialize();
  void Cleanup();
  
  const Executor& executor() const { return executor_; }
  ucp_worker_h GetWorker() const { return worker_; }
  uint64_t GenerateRequestId() { return ++request_id_counter_; }
  
  UcxRequestRegistry& GetRequestRegistry() { return *request_registry_; }
  
  void StartWorkerProgressTask();
  void StopWorkerProgressTask();
  
 private:
  EnhancedUcxManager() = default;
  ~EnhancedUcxManager() { Cleanup(); }
  
  void SchedulePeriodicTask();
  
  Executor executor_;
  ucp_context_h context_ = nullptr;
  ucp_worker_h worker_ = nullptr;
  std::atomic<bool> progress_task_running_{false};
  std::atomic<uint64_t> request_id_counter_{0};
  std::unique_ptr<UcxRequestRegistry> request_registry_;
  
  mutable absl::Mutex mutex_;
  bool initialized_ = false;
};

/// Request registry for tracking active UCX operations
class UcxRequestRegistry {
 public:
  void RegisterReadTask(uint64_t request_id, IntrusivePtr<UcxReadTask> task);
  void RegisterWriteTask(uint64_t request_id, IntrusivePtr<UcxWriteTask> task);
  
  IntrusivePtr<UcxReadTask> GetReadTask(uint64_t request_id);
  IntrusivePtr<UcxWriteTask> GetWriteTask(uint64_t request_id);
  
 private:
  absl::Mutex mutex_;
  absl::flat_hash_map<uint64_t, IntrusivePtr<UcxReadTask>> read_tasks_;
  absl::flat_hash_map<uint64_t, IntrusivePtr<UcxWriteTask>> write_tasks_;
};

/// Enhanced UCX Read Task with proper async patterns
struct UcxReadTask : public RateLimiterNode,
                     public AtomicReferenceCount<UcxReadTask> {
  IntrusivePtr<EnhancedRemoteDramDriver> owner;
  kvstore::Key key;
  kvstore::ReadOptions options;
  Promise<kvstore::ReadResult> promise;
  
  // UCX-specific state
  ucp_ep_h endpoint;
  uint64_t request_id;
  std::unique_ptr<UcxBuffer> send_buffer;
  std::unique_ptr<UcxBuffer> recv_buffer;
  int attempt = 0;
  absl::Time start_time;
  
  UcxReadTask(IntrusivePtr<EnhancedRemoteDramDriver> owner,
              kvstore::Key key,
              kvstore::ReadOptions options,
              Promise<kvstore::ReadResult> promise);
  
  ~UcxReadTask();
  
  static void Start(RateLimiterNode* task);
  static void Admit(RateLimiterNode* task);
  
  void Retry();
  void SendReadRequest();
  void OnSendComplete(ucs_status_t status);
  void OnReceiveComplete(const char* data, size_t size, ucs_status_t status);
  void CompleteWithError(absl::Status status);
  void CompleteWithResult(kvstore::ReadResult result);
};

/// Enhanced UCX Write Task with proper async patterns
struct UcxWriteTask : public RateLimiterNode,
                      public AtomicReferenceCount<UcxWriteTask> {
  IntrusivePtr<EnhancedRemoteDramDriver> owner;
  kvstore::Key key;
  absl::Cord value;
  kvstore::WriteOptions options;
  Promise<TimestampedStorageGeneration> promise;
  
  // UCX-specific state  
  ucp_ep_h endpoint;
  uint64_t request_id;
  std::unique_ptr<UcxBuffer> send_buffer;
  std::unique_ptr<UcxBuffer> recv_buffer;
  int attempt = 0;
  absl::Time start_time;
  
  UcxWriteTask(IntrusivePtr<EnhancedRemoteDramDriver> owner,
               kvstore::Key key,
               absl::Cord value,
               kvstore::WriteOptions options,
               Promise<TimestampedStorageGeneration> promise);
  
  ~UcxWriteTask();
  
  static void Start(RateLimiterNode* task);
  static void Admit(RateLimiterNode* task);
  
  void Retry();
  void SendWriteRequest();
  void OnSendComplete(ucs_status_t status);
  void OnReceiveComplete(const char* data, size_t size, ucs_status_t status);
  void CompleteWithError(absl::Status status);
  void CompleteWithResult(TimestampedStorageGeneration result);
};

/// Server-side task for handling read responses safely
struct ServerReadResponseTask : public AtomicReferenceCount<ServerReadResponseTask> {
  ucp_ep_h client_endpoint;
  uint64_t request_id;
  std::optional<absl::Cord> value;
  std::unique_ptr<UcxBuffer> send_buffer;
  
  ServerReadResponseTask(ucp_ep_h ep, uint64_t id, std::optional<absl::Cord> val);
  
  void Start();
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data);
};

/// Server-side task for handling write responses safely
struct ServerWriteResponseTask : public AtomicReferenceCount<ServerWriteResponseTask> {
  ucp_ep_h client_endpoint;
  uint64_t request_id;
  uint32_t status_code;
  std::unique_ptr<UcxBuffer> send_buffer;
  
  ServerWriteResponseTask(ucp_ep_h ep, uint64_t id, uint32_t status);
  
  void Start();
  static void ServerSendCallback(void* request, ucs_status_t status, void* user_data);
};

// UCX callback functions
void UcxSendCallback(void* request, ucs_status_t status, void* user_data);
void UcxReceiveCallback(void* request, ucs_status_t status, 
                        const ucp_tag_recv_info_t* info, void* user_data);

/// Enhanced Remote DRAM Driver with proper async patterns
class EnhancedRemoteDramDriver : public internal_kvstore::RegisteredDriver<
    EnhancedRemoteDramDriver, RemoteDramDriverSpec> {
 public:
  Future<kvstore::ReadResult> Read(kvstore::Key key, 
                                   kvstore::ReadOptions options) override;
  
  Future<TimestampedStorageGeneration> Write(kvstore::Key key,
                                             std::optional<kvstore::Value> value,
                                             kvstore::WriteOptions options) override;
  
  Future<kvstore::ReadResult> ReadImpl(kvstore::Key&& key,
                                       kvstore::ReadOptions&& options);
  
  Future<TimestampedStorageGeneration> WriteImpl(kvstore::Key&& key,
                                                 std::optional<kvstore::Value>&& value,
                                                 kvstore::WriteOptions&& options);
  
  // Rate limiting support
  RateLimiter& admission_queue() { return *spec_.request_concurrency->queue; }
  RateLimiter& read_rate_limiter() { return *spec_.rate_limiter->read_limiter; }
  RateLimiter& write_rate_limiter() { return *spec_.rate_limiter->write_limiter; }
  
  const Executor& executor() const {
    return spec_.data_copy_concurrency->executor;
  }
  
  // Retry support
  template <typename Task>
  absl::Status BackoffForAttemptAsync(absl::Status status, int attempt, Task* task);
  
 private:
  SpecData spec_;
  ucp_ep_h client_endpoint_ = nullptr;
  bool is_server_mode_ = false;
};

// Utility functions
bool IsRetriableStatus(const absl::Status& status);

}  // namespace remote_dram_enhanced
}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_FIX_H_