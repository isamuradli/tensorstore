# UCX Memory Management Fix Implementation Plan

## Executive Summary

The TensorStore remote_dram kvstore driver suffers from critical UCX memory management issues causing segmentation faults during read operations. This document provides a comprehensive implementation plan to fix these issues by adopting TensorStore's standard async patterns and UCX best practices.

## Problem Analysis

### Current Issues

1. **Segmentation Fault**: Server crashes in `uct_mm_ep_am_short+0xe4` during UCX worker progress polling
2. **Memory Race Condition**: UCX progress thread accesses deallocated buffers from send callbacks
3. **Improper Threading**: Uses `std::thread().detach()` instead of TensorStore's executor system
4. **Manual Memory Management**: Prone to leaks and use-after-free errors
5. **Non-Standard Architecture**: Doesn't follow TensorStore kvstore patterns

### Root Cause

The critical issue is in `SendReadResponse()` (lines 1177-1207):
- Creates `std::unique_ptr<char[]>` buffer and transfers ownership to callback
- UCX progress thread continues processing while buffer may be deallocated
- Race condition between progress thread and callback cleanup

## TensorStore Async Patterns Analysis

### Standard Task Architecture

All TensorStore network drivers use this pattern:

```cpp
struct ReadTask : public RateLimiterNode,
                  public internal::AtomicReferenceCount<ReadTask> {
  IntrusivePtr<DriverType> owner;
  Promise<kvstore::ReadResult> promise;
  kvstore::ReadOptions options;
  int attempt = 0;
  absl::Time start_time;
};
```

### Memory Management

- **Reference Counting**: `IntrusivePtr` and `AtomicReferenceCount`
- **RAII**: Automatic cleanup via destructors
- **Stable Buffers**: Buffers kept alive until async operations complete

### Threading Model

- **Executor Integration**: Uses TensorStore's executor system
- **Rate Limiting**: Built-in concurrency control
- **Cancellation**: Proper promise cancellation support

### Request Lifecycle

1. Create task with `MakeIntrusivePtr`
2. Register with rate limiter
3. Execute async operation
4. Handle completion via callbacks
5. Automatic cleanup via reference counting

## Implementation Plan

### Phase 1: Core Infrastructure Refactoring

#### 1.1 Replace Thread Management

**Current Problem:**
```cpp
std::thread([this]() {
  WorkerProgressTask();
}).detach();
```

**Solution:**
```cpp
class UcxManager {
  const Executor& executor() const {
    return executor_;
  }
  
private:
  Executor executor_;
  std::atomic<bool> progress_task_running_{false};
};

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_.exchange(true)) return;
  
  SchedulePeriodicTask();
}

void UcxManager::SchedulePeriodicTask() {
  if (!progress_task_running_.load()) return;
  
  executor()([this] {
    {
      absl::MutexLock lock(&mutex_);
      if (initialized_ && progress_task_running_.load()) {
        ucp_worker_progress(worker_);
      }
    }
    
    ScheduleAt(absl::Now() + absl::Microseconds(100),
               [this] { SchedulePeriodicTask(); });
  });
}
```

#### 1.2 Implement Task-Based Architecture

**UCX Read Task:**
```cpp
struct UcxReadTask : public RateLimiterNode,
                     public internal::AtomicReferenceCount<UcxReadTask> {
  IntrusivePtr<RemoteDramDriver> owner;
  kvstore::Key key;
  kvstore::ReadOptions options;
  Promise<kvstore::ReadResult> promise;
  
  // UCX-specific state
  ucp_ep_h endpoint;
  uint64_t request_id;
  std::unique_ptr<char[]> send_buffer;
  std::unique_ptr<char[]> recv_buffer;
  int attempt = 0;
  absl::Time start_time;
  
  UcxReadTask(IntrusivePtr<RemoteDramDriver> owner,
              kvstore::Key key,
              kvstore::ReadOptions options,
              Promise<kvstore::ReadResult> promise)
    : owner(std::move(owner)),
      key(std::move(key)),
      options(std::move(options)),
      promise(std::move(promise)),
      request_id(UcxManager::Instance().GenerateRequestId()) {}
  
  ~UcxReadTask() {
    owner->admission_queue().Finish(this);
  }
  
  static void Start(RateLimiterNode* task) {
    auto* self = static_cast<UcxReadTask*>(task);
    self->owner->read_rate_limiter().Finish(self);
    self->owner->admission_queue().Admit(self, &UcxReadTask::Admit);
  }
  
  static void Admit(RateLimiterNode* task) {
    auto* self = static_cast<UcxReadTask*>(task);
    self->owner->executor()(
        [state = IntrusivePtr<UcxReadTask>(self, internal::adopt_object_ref)] {
          state->Retry();
        });
  }
  
  void Retry() {
    if (!promise.result_needed()) return;
    
    start_time = absl::Now();
    SendReadRequest();
  }
  
  void SendReadRequest();
  void OnSendComplete(ucs_status_t status);
  void OnReceiveComplete(const char* data, size_t size, ucs_status_t status);
  void CompleteWithError(absl::Status status);
  void CompleteWithResult(kvstore::ReadResult result);
};
```

**UCX Write Task:**
```cpp
struct UcxWriteTask : public RateLimiterNode,
                      public internal::AtomicReferenceCount<UcxWriteTask> {
  IntrusivePtr<RemoteDramDriver> owner;
  kvstore::Key key;
  absl::Cord value;
  kvstore::WriteOptions options;
  Promise<TimestampedStorageGeneration> promise;
  
  // UCX-specific state  
  ucp_ep_h endpoint;
  uint64_t request_id;
  std::unique_ptr<char[]> send_buffer;
  std::unique_ptr<char[]> recv_buffer;
  int attempt = 0;
  absl::Time start_time;
  
  // Similar structure to ReadTask...
};
```

#### 1.3 UCX Request Management

**Request Registry:**
```cpp
class UcxRequestRegistry {
public:
  void RegisterReadTask(uint64_t request_id, IntrusivePtr<UcxReadTask> task) {
    absl::MutexLock lock(&mutex_);
    read_tasks_[request_id] = std::move(task);
  }
  
  void RegisterWriteTask(uint64_t request_id, IntrusivePtr<UcxWriteTask> task) {
    absl::MutexLock lock(&mutex_);
    write_tasks_[request_id] = std::move(task);
  }
  
  IntrusivePtr<UcxReadTask> GetReadTask(uint64_t request_id) {
    absl::MutexLock lock(&mutex_);
    auto it = read_tasks_.find(request_id);
    if (it != read_tasks_.end()) {
      auto task = it->second;
      read_tasks_.erase(it);
      return task;
    }
    return nullptr;
  }
  
  IntrusivePtr<UcxWriteTask> GetWriteTask(uint64_t request_id) {
    absl::MutexLock lock(&mutex_);
    auto it = write_tasks_.find(request_id);
    if (it != write_tasks_.end()) {
      auto task = it->second;
      write_tasks_.erase(it);
      return task;
    }
    return nullptr;
  }
  
private:
  absl::Mutex mutex_;
  absl::flat_hash_map<uint64_t, IntrusivePtr<UcxReadTask>> read_tasks_;
  absl::flat_hash_map<uint64_t, IntrusivePtr<UcxWriteTask>> write_tasks_;
};
```

### Phase 2: UCX Memory Management Fixes

#### 2.1 Proper Buffer Lifecycle Management

**Buffer Wrapper Class:**
```cpp
class UcxBuffer {
public:
  static std::unique_ptr<UcxBuffer> Create(size_t size) {
    return std::make_unique<UcxBuffer>(size);
  }
  
  explicit UcxBuffer(size_t size) : size_(size), data_(new char[size]) {}
  ~UcxBuffer() { delete[] data_; }
  
  char* data() { return data_; }
  const char* data() const { return data_; }
  size_t size() const { return size_; }
  
private:
  size_t size_;
  char* data_;
};
```

#### 2.2 Safe UCX Callback Pattern

**Send Callback:**
```cpp
void UcxSendCallback(void* request, ucs_status_t status, void* user_data) {
  auto* task_ptr = static_cast<UcxReadTask*>(user_data);
  auto task = IntrusivePtr<UcxReadTask>(task_ptr, internal::adopt_object_ref);
  
  // Schedule completion on proper executor to avoid deadlock
  task->owner->executor()([task = std::move(task), status] {
    task->OnSendComplete(status);
  });
  
  ucp_request_free(request);
}
```

**Receive Callback:**
```cpp
void UcxReceiveCallback(void* request, ucs_status_t status, 
                        const ucp_tag_recv_info_t* info, void* user_data) {
  auto* task_ptr = static_cast<UcxReadTask*>(user_data);
  auto task = IntrusivePtr<UcxReadTask>(task_ptr, internal::adopt_object_ref);
  
  task->owner->executor()([task = std::move(task), status, 
                          length = info->length] {
    task->OnReceiveComplete(task->recv_buffer->data(), length, status);
  });
  
  ucp_request_free(request);
}
```

#### 2.3 UCX Task Implementation

**Read Task Send Implementation:**
```cpp
void UcxReadTask::SendReadRequest() {
  // Create message
  size_t message_size = sizeof(MessageHeader) + key.size();
  send_buffer = UcxBuffer::Create(message_size);
  
  // Initialize header and payload
  MessageHeader* header = reinterpret_cast<MessageHeader*>(send_buffer->data());
  message_utils::InitializeMessageHeader(
      header, MessageType::READ_REQUEST, 
      static_cast<uint32_t>(key.size()), 0, request_id,
      send_buffer->data() + sizeof(MessageHeader));
  
  std::memcpy(send_buffer->data() + sizeof(MessageHeader), 
              key.data(), key.size());
  
  // Allocate receive buffer
  recv_buffer = UcxBuffer::Create(64 * 1024);  // 64KB max
  
  // Register this task for callbacks
  UcxManager::Instance().GetRequestRegistry().RegisterReadTask(request_id, 
      IntrusivePtr<UcxReadTask>(this));
  
  // Post receive first
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = UcxReceiveCallback;
  recv_params.user_data = this;
  intrusive_ptr_increment(this);  // For receive callback
  
  void* recv_request = ucp_tag_recv_nbx(
      UcxManager::Instance().GetWorker(),
      recv_buffer->data(), recv_buffer->size(),
      UCX_TAG_READ_RESPONSE, UCX_TAG_MASK, &recv_params);
  
  if (UCS_PTR_IS_ERR(recv_request)) {
    intrusive_ptr_decrement(this);  // Cleanup receive ref
    CompleteWithError(absl::InternalError("Failed to post receive"));
    return;
  }
  
  // Send request
  ucp_request_param_t send_params;
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  send_params.cb.send = UcxSendCallback;
  send_params.user_data = this;
  intrusive_ptr_increment(this);  // For send callback
  
  void* send_request = ucp_tag_send_nbx(
      endpoint, send_buffer->data(), send_buffer->size(),
      UCX_TAG_READ_REQUEST, &send_params);
  
  if (UCS_PTR_IS_ERR(send_request)) {
    intrusive_ptr_decrement(this);  // Cleanup send ref
    CompleteWithError(absl::InternalError("Failed to send request"));
    return;
  }
}
```

**Completion Handlers:**
```cpp
void UcxReadTask::OnSendComplete(ucs_status_t status) {
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Send failed: " << ucs_status_string(status);
    // Don't fail immediately - wait for receive timeout or completion
  }
  
  // Send buffer no longer needed
  send_buffer.reset();
}

void UcxReadTask::OnReceiveComplete(const char* data, size_t size, ucs_status_t status) {
  if (!promise.result_needed()) {
    return;  // Operation was cancelled
  }
  
  if (status != UCS_OK) {
    CompleteWithError(absl::InternalError(
        absl::StrFormat("Receive failed: %s", ucs_status_string(status))));
    return;
  }
  
  if (size < sizeof(ReadResponse)) {
    CompleteWithError(absl::InternalError("Invalid response size"));
    return;
  }
  
  // Parse response
  const ReadResponse* response = reinterpret_cast<const ReadResponse*>(data);
  
  kvstore::ReadResult result;
  if (response->status_code == 0 && response->header.value_length > 0) {
    // Success with value
    result.state = kvstore::ReadResult::kValue;
    const char* value_ptr = data + sizeof(ReadResponse);
    result.value = absl::Cord(std::string(value_ptr, response->header.value_length));
    result.stamp.generation = StorageGeneration::FromString("remote_read");
    result.stamp.time = start_time;
  } else {
    // Key not found
    result.state = kvstore::ReadResult::kMissing;
    result.stamp.generation = StorageGeneration::NoValue();
    result.stamp.time = start_time;
  }
  
  CompleteWithResult(std::move(result));
}

void UcxReadTask::CompleteWithResult(kvstore::ReadResult result) {
  promise.SetResult(std::move(result));
}

void UcxReadTask::CompleteWithError(absl::Status status) {
  // Handle retries
  if (IsRetriableStatus(status)) {
    auto retry_status = owner->BackoffForAttemptAsync(status, attempt_++, this);
    if (retry_status.ok()) {
      return;  // Will retry
    }
  }
  
  // Create error result
  kvstore::ReadResult error_result;
  error_result.state = kvstore::ReadResult::kMissing;
  error_result.stamp.generation = StorageGeneration::NoValue();
  error_result.stamp.time = start_time;
  
  promise.SetResult(std::move(error_result));
}
```

### Phase 3: Driver Integration

#### 3.1 Update RemoteDramDriver

```cpp
class RemoteDramDriver : public internal_kvstore::RegisteredDriver<...> {
public:
  Future<kvstore::ReadResult> Read(kvstore::Key key, 
                                   kvstore::ReadOptions options) override {
    if (is_server_mode_) {
      return ReadLocal(key, options);
    }
    
    // Use batch processing framework
    return internal_kvstore_batch::HandleBatchRequestByGenericByteRangeCoalescing(
        *this, std::move(key), std::move(options));
  }
  
  Future<kvstore::ReadResult> ReadImpl(kvstore::Key&& key,
                                       kvstore::ReadOptions&& options) {
    auto op = PromiseFuturePair<kvstore::ReadResult>::Make();
    auto task = internal::MakeIntrusivePtr<UcxReadTask>(
        internal::IntrusivePtr<RemoteDramDriver>(this),
        std::move(key), std::move(options), std::move(op.promise));
    
    task->endpoint = client_endpoint_;
    
    intrusive_ptr_increment(task.get());
    read_rate_limiter().Admit(task.get(), &UcxReadTask::Start);
    
    return std::move(op.future);
  }
  
  // Rate limiting support
  RateLimiter& admission_queue() { return *spec_.request_concurrency->queue; }
  RateLimiter& read_rate_limiter() { return *spec_.rate_limiter->read_limiter; }
  RateLimiter& write_rate_limiter() { return *spec_.rate_limiter->write_limiter; }
  
  const Executor& executor() const {
    return spec_.data_copy_concurrency->executor;
  }
  
  // Retry support
  template <typename Task>
  absl::Status BackoffForAttemptAsync(absl::Status status, int attempt, Task* task) {
    auto delay = spec_.retries->BackoffForAttempt(attempt);
    if (!delay) {
      return MaybeAnnotateStatus(std::move(status), "All retry attempts failed");
    }
    ScheduleAt(absl::Now() + *delay, WithExecutor(executor(), [task] {
      task->Retry();
    }));
    return absl::OkStatus();
  }

private:
  SpecData spec_;
  ucp_ep_h client_endpoint_ = nullptr;
  bool is_server_mode_ = false;
};
```

#### 3.2 Server-Side Improvements

**Server Task for Read Responses:**
```cpp
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
    
    // Send response
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
```

### Phase 4: Testing and Validation

#### 4.1 Unit Tests

**Task Lifecycle Tests:**
```cpp
TEST(UcxReadTaskTest, BasicLifecycle) {
  // Test task creation, execution, and cleanup
}

TEST(UcxReadTaskTest, CancellationHandling) {
  // Test proper cleanup when operations are cancelled
}

TEST(UcxReadTaskTest, RetryLogic) {
  // Test retry mechanism with various error conditions
}
```

**Memory Management Tests:**
```cpp
TEST(UcxBufferTest, LifecycleManagement) {
  // Test buffer creation, usage, and automatic cleanup
}

TEST(UcxCallbackTest, ThreadSafety) {
  // Test callback handling across different threads
}
```

#### 4.2 Integration Tests

**End-to-End Tests:**
```cpp
TEST(RemoteDramDriverTest, ReadWriteOperations) {
  // Test complete read/write cycles
}

TEST(RemoteDramDriverTest, ConcurrentOperations) {
  // Test multiple concurrent operations
}

TEST(RemoteDramDriverTest, ErrorRecovery) {
  // Test error handling and recovery
}
```

### Phase 5: Performance Optimization

#### 5.1 Batch Processing

Implement TensorStore's batch processing framework:
```cpp
internal_kvstore_batch::CoalescingOptions GetBatchReadCoalescingOptions() const {
  return internal_kvstore_batch::kDefaultRemoteStorageCoalescingOptions;
}
```

#### 5.2 Connection Pooling

Implement UCX endpoint pooling for better performance:
```cpp
class UcxEndpointPool {
  // Pool management for UCX endpoints
};
```

#### 5.3 Memory Pool

Implement buffer pooling to reduce allocation overhead:
```cpp
class UcxBufferPool {
  // Reusable buffer management
};
```

## Migration Strategy

### Phase 1: Infrastructure (Week 1-2)
1. Implement task-based architecture
2. Replace thread management
3. Add executor integration

### Phase 2: Memory Management (Week 2-3)
1. Implement safe buffer management
2. Fix UCX callback patterns
3. Add request registry

### Phase 3: Driver Integration (Week 3-4)
1. Update RemoteDramDriver
2. Implement rate limiting
3. Add retry logic

### Phase 4: Testing (Week 4-5)
1. Unit tests
2. Integration tests
3. Performance validation

### Phase 5: Optimization (Week 5-6)
1. Batch processing
2. Connection pooling
3. Performance tuning

## Expected Outcomes

### Immediate Benefits
- **Crash Elimination**: No more segmentation faults
- **Memory Safety**: Proper RAII and reference counting
- **Thread Safety**: Correct executor usage

### Long-term Benefits
- **Performance**: Rate limiting and batch processing
- **Reliability**: Standardized retry mechanisms
- **Maintainability**: Consistent with TensorStore patterns
- **Scalability**: Proper concurrency control

## Risk Assessment

### Low Risk
- Task architecture adoption (well-established pattern)
- Memory management fixes (clear root cause)

### Medium Risk  
- UCX callback integration (requires careful testing)
- Threading model changes (potential performance impact)

### Mitigation Strategies
- Comprehensive testing at each phase
- Gradual rollout with fallback options
- Performance benchmarking throughout

## Conclusion

This implementation plan addresses the critical UCX memory management issues by adopting TensorStore's proven async patterns. The systematic approach ensures both immediate crash fixes and long-term maintainability while maintaining compatibility with the existing TensorStore architecture.

The key insight is that the current implementation tries to manage UCX resources manually, leading to race conditions. By adopting TensorStore's task-based architecture with proper reference counting and executor integration, we eliminate these issues while gaining the benefits of the mature TensorStore infrastructure.