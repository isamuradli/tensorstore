# Phase 1: UCX Foundation Optimization - Detailed Implementation Plan

## Overview
**Timeline**: Weeks 1-3  
**Goal**: Fix core UCX architecture and eliminate primary performance bottlenecks  
**Expected Improvement**: 10-50x throughput increase (14MB/s → 140MB/s - 700MB/s)

## Week 1: UCX Multi-Worker Architecture

### **Task 1.1: UCX Worker Pool Creation**
**Priority**: CRITICAL  
**Location**: Replace `UcxManager` singleton with worker pool

#### Current Issue
```cpp
// BOTTLENECK: Single UCX worker serializes all operations
class UcxManager {
  ucp_worker_h worker_;  // ← Single worker bottleneck
  std::thread progress_thread_;  // ← Single progress thread
  absl::Mutex mutex_;  // ← Global serialization
};
```

#### Implementation
```cpp
struct UcxWorkerContext {
  ucp_context_h context;
  ucp_worker_h worker;
  ucp_ep_h endpoint;
  std::thread progress_thread;
  std::atomic<bool> active{true};
  std::atomic<uint64_t> next_request_id{1};
  uint8_t worker_id;
  
  void StartProgressThread() {
    progress_thread = std::thread([this]() { WorkerProgressTask(); });
  }
  
private:
  void WorkerProgressTask() {
    while (active.load(std::memory_order_acquire)) {
      ucp_worker_progress(worker);  // No global locks
      std::this_thread::sleep_for(std::chrono::microseconds(1));  // 100x faster
    }
  }
};

class UcxWorkerPool {
  std::vector<std::unique_ptr<UcxWorkerContext>> workers_;
  std::atomic<size_t> next_worker_index_{0};
  
public:
  void Initialize(size_t num_workers = 4) {
    for (size_t i = 0; i < num_workers; ++i) {
      auto worker = CreateWorker(i);
      worker->StartProgressThread();
      workers_.push_back(std::move(worker));
    }
  }
  
  UcxWorkerContext* SelectWorker() {
    size_t index = next_worker_index_.fetch_add(1) % workers_.size();
    return workers_[index].get();
  }
};
```

#### Performance Impact
```
Current: Single worker = max 10,000 ops/sec (100μs sleep)
New: 4 workers × 1,000,000 ops/sec (1μs sleep) = 4M ops/sec theoretical
Expected: 10-50x improvement
```

### **Task 1.2: Lock-Free Request ID Generation**
**Priority**: HIGH  
**Location**: Eliminate global mutex for request IDs

#### Current Serialization Problem
```cpp
uint64_t UcxManager::GenerateRequestId() {
  absl::MutexLock lock(&mutex_);  // ← SERIALIZATION BOTTLENECK
  return next_request_id_++;
}
```

#### Lock-Free Per-Worker Implementation
```cpp
struct UcxWorkerContext {
  std::atomic<uint64_t> next_request_id{1};
  uint8_t worker_id;  // Set during initialization
  
  uint64_t GenerateRequestId() {
    // Lock-free, globally unique IDs
    uint64_t sequence = next_request_id.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<uint64_t>(worker_id) << 56) | sequence;
  }
};
```

#### Worker-Specific Send Implementation  
```cpp
void UcxWorkerContext::SendWriteRequest(const std::string& key, const absl::Cord& value) {
  uint64_t request_id = GenerateRequestId();  // Lock-free per worker
  
  auto buffer = AllocateBuffer(key.size() + value.size());
  ConstructWriteMessage(buffer, request_id, key, value);
  
  ucp_request_param_t params{};
  params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  params.cb.send = &OnUcxSendCompleted;
  params.user_data = reinterpret_cast<void*>(request_id);
  
  ucp_tag_send_nbx(endpoint, buffer->data(), buffer->size(), 
                   UCX_TAG_WRITE_REQUEST, &params);
}
```

### **Task 1.3: Context Resource Management**
**Priority**: CRITICAL  
**Location**: Replace `UcxManager` singleton pattern

#### Current Singleton Problem
```cpp
// Anti-pattern: Singleton usage
UcxManager::Instance().SendWriteRequest(...);
```

#### New Context Resource Pattern
```cpp
struct RemoteDramConcurrencyResource
    : public internal::ConcurrencyResource<RemoteDramConcurrencyResource> {
  constexpr static char id[] = "remote_dram_concurrency";
  
  struct Spec {
    size_t max_workers = 4;
    size_t max_outstanding_per_worker = 64;
  };
  
  constexpr static auto default_json_binder = jb::Object(
    jb::Member("max_workers", jb::Projection<&Spec::max_workers>()),
    jb::Member("max_outstanding_per_worker", 
               jb::Projection<&Spec::max_outstanding_per_worker>())
  );
  
  using Resource = RemoteDramConcurrencyResource;
  using Spec = Spec;
};

// Registration
const internal::ContextResourceRegistration<RemoteDramConcurrencyResource>
    remote_dram_concurrency_registration;
```

#### UcxManager Replacement
```cpp
class UcxResourceManager {
  Context::Resource<RemoteDramConcurrencyResource> concurrency_resource_;
  std::vector<std::unique_ptr<ClientWorkerContext>> workers_;
  
public:
  explicit UcxResourceManager(Context::Resource<RemoteDramConcurrencyResource> resource)
      : concurrency_resource_(std::move(resource)) {
    InitializeWorkers();
  }
  
  ClientWorkerContext* SelectWorker() {
    return workers_[worker_round_robin_.fetch_add(1) % workers_.size()].get();
  }
};
```

## Week 2: UCX Progress Optimization

### **Task 2.1: Multi-Worker Architecture**
**Priority**: CRITICAL - Primary performance bottleneck  
**Location**: Replace single worker in `UcxManager`

#### Current Bottleneck
```cpp
void UcxManager::WorkerProgressTask() {
  while (true) {
    {
      absl::MutexLock lock(&mutex_);  // ← SERIALIZATION BOTTLENECK
      ucp_worker_progress(worker_);   // ← SINGLE WORKER BOTTLENECK
    }  
    std::this_thread::sleep_for(std::chrono::microseconds(100));  // ← LATENCY BOTTLENECK
  }
}
```

#### New Multi-Worker Implementation
```cpp
struct ClientWorkerContext {
  ucp_context_h context;
  ucp_worker_h worker;
  ucp_ep_h endpoint;
  std::thread progress_thread;
  std::atomic<bool> active{true};
  std::atomic<uint64_t> next_request_id{1};
  std::atomic<size_t> outstanding_operations{0};
  
  static constexpr size_t MAX_OUTSTANDING = 64;
  
  void StartProgressThread() {
    progress_thread = std::thread([this]() { WorkerProgressTask(); });
  }
  
private:
  void WorkerProgressTask() {
    while (active.load(std::memory_order_acquire)) {
      // No global mutex needed - each worker independent
      ucp_worker_progress(worker);
      
      // 100x faster polling - 1μs vs 100μs
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }
};
```

#### Worker Pool Management
```cpp
class UcxWorkerPool {
  std::vector<std::unique_ptr<ClientWorkerContext>> workers_;
  std::atomic<size_t> next_worker_index_{0};
  
public:
  void Initialize(size_t num_workers, const std::string& server_addr) {
    workers_.reserve(num_workers);
    
    for (size_t i = 0; i < num_workers; ++i) {
      auto worker = std::make_unique<ClientWorkerContext>();
      InitializeWorker(worker.get(), server_addr);
      worker->StartProgressThread();
      workers_.push_back(std::move(worker));
    }
  }
  
  ClientWorkerContext* SelectWorker() {
    // Round-robin load balancing
    size_t index = next_worker_index_.fetch_add(1, std::memory_order_relaxed);
    return workers_[index % workers_.size()].get();
  }
};
```

#### Performance Impact Calculation
```
Current: Single worker with 100μs sleep = 10,000 ops/sec max
New: 4 workers with 1μs sleep = 4,000,000 ops/sec theoretical
Expected: 10-50x throughput improvement
```

### **Task 2.2: Lock-Free Request ID Generation**
**Priority**: HIGH  
**Location**: Replace `UcxManager::GenerateRequestId()`

#### Current Serialization
```cpp
uint64_t UcxManager::GenerateRequestId() {
  absl::MutexLock lock(&mutex_);  // ← SERIALIZATION BOTTLENECK
  return next_request_id_++;
}
```

#### Lock-Free Per-Worker IDs
```cpp
struct ClientWorkerContext {
  std::atomic<uint64_t> next_request_id{1};
  uint8_t worker_id;  // Set during initialization
  
  uint64_t GenerateRequestId() {
    // Lock-free, globally unique IDs
    uint64_t sequence = next_request_id.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<uint64_t>(worker_id) << 56) | sequence;
  }
};
```

### **Task 2.3: Flow Control Implementation**
**Priority**: HIGH  
**Location**: Prevent resource exhaustion

#### Sliding Window Flow Control
```cpp
struct ClientWorkerContext {
  std::atomic<size_t> outstanding_operations{0};
  static constexpr size_t MAX_OUTSTANDING = 64;
  
  bool CanSendOperation() const {
    return outstanding_operations.load(std::memory_order_acquire) < MAX_OUTSTANDING;
  }
  
  void IncrementOutstanding() {
    outstanding_operations.fetch_add(1, std::memory_order_release);
  }
  
  void DecrementOutstanding() {
    outstanding_operations.fetch_sub(1, std::memory_order_release);
  }
};
```

## Week 3: Enterprise Error Handling

### **Task 3.1: Sophisticated Retry Logic**
**Priority**: HIGH  
**Location**: Replace basic retry in `UcxRetryManager`

#### Current Basic Retry
```cpp
class UcxRetryManager {
  static constexpr int kMaxRetries = 3;
  static constexpr absl::Duration kBaseDelay = absl::Milliseconds(100);
};
```

#### Enterprise Retry with Jitter
```cpp
class EnterpriseRetryManager {
  struct RetryState {
    int attempt_count = 0;
    absl::Time initial_attempt_time = absl::Now();
    absl::Duration current_delay = absl::Milliseconds(10);
  };
  
  static constexpr int kMaxRetries = 10;
  static constexpr absl::Duration kMaxDelay = absl::Seconds(30);
  static constexpr double kBackoffMultiplier = 2.0;
  static constexpr double kJitterFraction = 0.1;
  
public:
  bool ShouldRetry(ucs_status_t status, RetryState& state) {
    if (state.attempt_count >= kMaxRetries) return false;
    if (!IsRetriableError(status)) return false;
    if (absl::Now() - state.initial_attempt_time > absl::Minutes(5)) return false;
    
    return true;
  }
  
  absl::Duration CalculateBackoff(RetryState& state) {
    state.attempt_count++;
    
    // Exponential backoff with jitter
    absl::Duration delay = std::min(state.current_delay, kMaxDelay);
    double jitter = 1.0 + (absl::ToDoubleSeconds(absl::Now() - absl::UnixEpoch()) * kJitterFraction);
    delay *= jitter;
    
    state.current_delay *= kBackoffMultiplier;
    return delay;
  }
  
private:
  bool IsRetriableError(ucs_status_t status) {
    switch (status) {
      case UCS_ERR_NO_MEMORY:
      case UCS_ERR_BUSY:
      case UCS_ERR_NO_RESOURCE:
      case UCS_ERR_ENDPOINT_TIMEOUT:
        return true;
      case UCS_ERR_CONNECTION_RESET:
      case UCS_ERR_UNREACHABLE:
      case UCS_ERR_INVALID_PARAM:
        return false;
      default:
        return false;
    }
  }
};
```

### **Task 3.2: Memory Safety Completion**
**Priority**: HIGH  
**Location**: Eliminate remaining manual memory management

#### RAII Buffer Management
```cpp
class UcxBuffer {
  void* data_;
  size_t size_;
  ucp_mem_h memory_handle_;
  
public:
  UcxBuffer(size_t size, ucp_context_h context) : size_(size) {
    ucp_mem_map_params_t params{};
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                       UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    params.length = size;
    
    ucs_status_t status = ucp_mem_map(context, &params, &memory_handle_);
    if (status != UCS_OK) {
      throw std::runtime_error("Failed to map UCX memory");
    }
    
    ucp_mem_attr_t attr{};
    attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS;
    ucp_mem_query(memory_handle_, &attr);
    data_ = attr.address;
  }
  
  ~UcxBuffer() {
    if (memory_handle_) {
      ucp_mem_unmap(ucp_context_, memory_handle_);
    }
  }
  
  void* data() const { return data_; }
  size_t size() const { return size_; }
};
```

## Phase 1 Success Metrics

### **Performance Targets**
- **Throughput**: 140MB/s - 700MB/s (10-50x improvement from 14MB/s)
- **Latency**: 1-10ms per operation (10-100x improvement from ~100ms)
- **Concurrency**: 4 parallel workers operating independently

### **Compliance Targets**
- [ ] Driver discoverable via `kvstore::Open({{"driver", "remote_dram"}})`
- [ ] Task-based architecture with proper RAII memory management
- [ ] Context resource management (no singletons)
- [ ] Enterprise retry logic with jitter and proper error classification

### **Validation Tests**
```cpp
TEST(Phase1ValidationTest, BasicPerformance) {
  // Validate 10x minimum improvement
  auto throughput = MeasureThroughput(1000, 16*1024);  // 1000 x 16KB writes
  EXPECT_GE(throughput, 140e6);  // 140 MB/s minimum
}

TEST(Phase1ValidationTest, TensorStoreCompliance) {
  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store, kvstore::Open({{"driver", "remote_dram"}, 
                                {"remote_addr", "192.168.1.100:8888"}}, 
                               context).result());
  
  // Validate independent operations
  std::vector<Future<TimestampedStorageGeneration>> futures;
  for (int i = 0; i < 100; ++i) {
    futures.push_back(kvstore::Write(store, absl::StrCat("key", i), 
                                    absl::Cord(std::string(16*1024, 'x'))));
  }
  
  // All operations should complete independently
  for (auto& f : futures) {
    TENSORSTORE_ASSERT_OK(f.result());
  }
}
```

Phase 1 establishes the foundation for dramatic performance improvements in subsequent phases by eliminating the primary serialization bottlenecks and ensuring full TensorStore framework compatibility.