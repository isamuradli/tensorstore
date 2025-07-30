# Phase 2: UCX Zero-Copy & Scaling - Detailed Implementation Plan

## Overview
**Timeline**: Weeks 4-6  
**Prerequisites**: Phase 1 completion (UCX multi-worker architecture + memory pools)  
**Goal**: Achieve target 1-5 GB/s throughput through UCX-optimized zero-copy and scaling  
**Expected Improvement**: Additional 10-100x on top of Phase 1 (700MB/s → 7GB/s - 70GB/s)

## Week 4: UCX Zero-Copy Implementation

### **Task 4.1: Direct UCX Buffer Construction**
**Priority**: CRITICAL - Eliminates memory copying overhead  
**Location**: Message construction throughout system

#### Current Triple Copy Problem
```cpp
// COPY #1: absl::Cord to std::string (16KB → 16KB)
std::string value_content = std::string(value);

// COPY #2: std::string to message buffer (16KB → 16KB)  
std::memcpy(data_ptr + key.size(), value_content.data(), value_content.size());

// COPY #3: message buffer to stable buffer (32KB → 32KB)
char* stable_buffer = new char[message_size];
std::memcpy(stable_buffer, message_buffer.data(), message_size);

// Total: 48KB memory traffic for 16KB write (3x overhead)
```

#### UCX Zero-Copy Direct Construction
```cpp
class UcxZeroCopyMessageBuilder {
  std::unique_ptr<UcxBuffer> buffer_;
  WriteMessage* message_;
  char* data_region_;
  
public:
  static std::unique_ptr<UcxZeroCopyMessageBuilder> CreateWriteMessage(
      UcxWorkerContext* worker, const std::string& key, const absl::Cord& value) {
    
    size_t total_size = sizeof(WriteMessage) + key.size() + value.size();
    
    // Allocate directly in UCX registered memory (no copies needed)
    auto buffer = worker->GetMemoryPool()->AllocateBuffer(total_size);
    auto builder = std::make_unique<UcxZeroCopyMessageBuilder>();
    builder->buffer_ = std::move(buffer);
    
    // Construct message header directly in UCX memory
    builder->message_ = reinterpret_cast<WriteMessage*>(builder->buffer_->data());
    builder->data_region_ = reinterpret_cast<char*>(builder->message_ + 1);
    
    // Copy key and value directly to final location (single copy only)
    std::memcpy(builder->data_region_, key.data(), key.size());
    value.CopyToArray(builder->data_region_ + key.size());  // Direct from Cord
    
    // Initialize header
    builder->message_->header.magic_number = 0xDEADBEEF;
    builder->message_->header.type = MessageType::WRITE_REQUEST;
    builder->message_->header.key_length = key.size();
    builder->message_->header.value_length = value.size();
    builder->message_->header.request_id = worker->GenerateRequestId();
    builder->message_->header.checksum = CalculateChecksum(builder->data_region_, 
                                                          key.size() + value.size());
    
    return builder;
  }
  
  UcxBuffer* GetBuffer() const { return buffer_.get(); }
  size_t GetTotalSize() const { 
    return sizeof(WriteMessage) + message_->header.key_length + message_->header.value_length; 
  }
};
```

#### Performance Impact Calculation
```
Current: 3x memory bandwidth (48KB for 16KB write)
Zero-copy: 1x memory bandwidth (16KB for 16KB write)
Memory bandwidth reduction: 67%
Expected throughput improvement: 2-3x
```

### **Task 4.2: Background Reliability Tracking**
**Priority**: CRITICAL - Prevent data loss in fire-and-forget  
**Location**: New reliability subsystem

#### Reliability Tracker Implementation
```cpp
class ReliabilityTracker {
  struct PendingOperation {
    std::string key;
    uint64_t generation;
    absl::Time start_time;
    int retry_count = 0;
    std::weak_ptr<RemoteDramFireAndForgetWrite> original_task;
  };
  
  absl::Mutex mutex_;
  absl::flat_hash_map<std::string, PendingOperation> pending_operations_ ABSL_GUARDED_BY(mutex_);
  std::thread reliability_thread_;
  std::atomic<bool> active_{true};
  
public:
  void TrackOperation(const std::string& key, uint64_t generation, 
                     std::shared_ptr<RemoteDramFireAndForgetWrite> task) {
    absl::MutexLock lock(&mutex_);
    pending_operations_[key] = PendingOperation{
      .key = key,
      .generation = generation, 
      .start_time = absl::Now(),
      .retry_count = 0,
      .original_task = task
    };
  }
  
  void MarkCompleted(const std::string& key, uint64_t generation) {
    absl::MutexLock lock(&mutex_);
    pending_operations_.erase(key);
  }
  
  void MarkFailed(const std::string& key, uint64_t generation, ucs_status_t status) {
    absl::MutexLock lock(&mutex_);
    if (auto it = pending_operations_.find(key); it != pending_operations_.end()) {
      it->second.retry_count++;
      // Will be handled by background reliability thread
    }
  }
  
private:
  void ReliabilityMonitoringLoop() {
    while (active_.load()) {
      ProcessTimeouts();
      ProcessRetries();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  
  void ProcessTimeouts() {
    absl::MutexLock lock(&mutex_);
    auto now = absl::Now();
    
    for (auto it = pending_operations_.begin(); it != pending_operations_.end();) {
      if (now - it->second.start_time > absl::Seconds(30)) {
        // Operation timeout - schedule retry or report failure
        if (it->second.retry_count < 3) {
          ScheduleRetry(it->second);
          ++it;
        } else {
          // Permanent failure - log and remove
          LOG(ERROR) << "Operation permanently failed: " << it->first;
          it = pending_operations_.erase(it);
        }
      } else {
        ++it;
      }
    }
  }
};
```

### **Task 4.3: Advanced Flow Control**
**Priority**: HIGH - Prevent resource exhaustion  
**Location**: Enhanced worker flow control

#### Adaptive Flow Control
```cpp
struct AdaptiveFlowControl {
  std::atomic<size_t> outstanding_operations{0};
  std::atomic<size_t> max_outstanding{64};  // Dynamic limit
  std::atomic<double> success_rate{1.0};
  std::atomic<absl::Time> last_adjustment{absl::Now()};
  
  bool CanSendOperation() const {
    size_t current_outstanding = outstanding_operations.load(std::memory_order_acquire);
    size_t current_limit = max_outstanding.load(std::memory_order_acquire);
    return current_outstanding < current_limit;
  }
  
  void AdaptLimits(double recent_success_rate) {
    if (absl::Now() - last_adjustment.load() < absl::Seconds(1)) return;
    
    size_t current_limit = max_outstanding.load();
    
    if (recent_success_rate > 0.95 && current_limit < 256) {
      // Increase limit on high success rate
      max_outstanding.store(std::min(current_limit * 2, size_t(256)));
    } else if (recent_success_rate < 0.8 && current_limit > 16) {
      // Decrease limit on high failure rate
      max_outstanding.store(std::max(current_limit / 2, size_t(16)));
    }
    
    last_adjustment.store(absl::Now());
  }
};
```

## Week 5: Zero-Copy Memory Management

### **Task 5.1: Direct UCX Buffer Construction**
**Priority**: CRITICAL - Eliminates triple copying  
**Location**: Message construction throughout system

#### Current Triple Copy Problem
```cpp
// COPY #1: absl::Cord to std::string (16KB → 16KB)
std::string value_content = std::string(value);

// COPY #2: std::string to message buffer (16KB → 16KB)  
std::memcpy(data_ptr + key.size(), value_content.data(), value_content.size());

// COPY #3: message buffer to stable buffer (32KB → 32KB)
char* stable_buffer = new char[message_size];
std::memcpy(stable_buffer, message_buffer.data(), message_size);

// Total: 48KB memory traffic for 16KB write (3x overhead)
```

#### Zero-Copy Direct Construction
```cpp
class ZeroCopyMessageBuilder {
  std::unique_ptr<UcxBuffer> buffer_;
  WriteMessage* message_;
  char* data_region_;
  
public:
  static std::unique_ptr<ZeroCopyMessageBuilder> CreateWriteMessage(
      ClientWorkerContext* worker, const std::string& key, const absl::Cord& value) {
    
    size_t total_size = sizeof(WriteMessage) + key.size() + value.size();
    
    // Allocate directly in UCX registered memory (no copies needed)
    auto buffer = worker->AllocateRegisteredBuffer(total_size);
    auto builder = std::make_unique<ZeroCopyMessageBuilder>();
    builder->buffer_ = std::move(buffer);
    
    // Construct message header directly in UCX memory
    builder->message_ = reinterpret_cast<WriteMessage*>(builder->buffer_->data());
    builder->data_region_ = reinterpret_cast<char*>(builder->message_ + 1);
    
    // Copy key and value directly to final location (single copy only)
    std::memcpy(builder->data_region_, key.data(), key.size());
    value.CopyToArray(builder->data_region_ + key.size());  // Direct from Cord
    
    // Initialize header
    builder->message_->header.magic_number = 0xDEADBEEF;
    builder->message_->header.type = MessageType::WRITE_REQUEST;
    builder->message_->header.key_length = key.size();
    builder->message_->header.value_length = value.size();
    builder->message_->header.request_id = worker->GenerateRequestId();
    builder->message_->header.checksum = CalculateChecksum(builder->data_region_, 
                                                          key.size() + value.size());
    
    return builder;
  }
  
  UcxBuffer* GetBuffer() const { return buffer_.get(); }
  size_t GetTotalSize() const { 
    return sizeof(WriteMessage) + message_->header.key_length + message_->header.value_length; 
  }
};
```

#### Performance Impact Calculation
```
Current: 3x memory bandwidth (48KB for 16KB write)
Zero-copy: 1x memory bandwidth (16KB for 16KB write)
Memory bandwidth reduction: 67%
Expected throughput improvement: 2-3x
```

### **Task 5.2: UCX Memory Pool Implementation**
**Priority**: HIGH - Eliminates registration overhead  
**Location**: New memory management subsystem

#### Pre-Registered Memory Pool
```cpp
class UcxMemoryPool {
  struct BufferInfo {
    void* data;
    size_t size;
    ucp_mem_h memory_handle;
    std::atomic<bool> in_use{false};
  };
  
  ucp_context_h context_;
  std::vector<BufferInfo> buffer_pool_;
  std::atomic<size_t> next_buffer_index_{0};
  
  // Standard buffer sizes optimized for common workloads
  static constexpr std::array<size_t, 6> BUFFER_SIZES = {
    1024,     // Small messages
    4096,     // Medium messages  
    16384,    // Typical chunk size
    65536,    // Large messages
    262144,   // Very large messages
    1048576   // Maximum message size
  };
  
public:
  explicit UcxMemoryPool(ucp_context_h context, size_t pool_size_per_category = 32) 
      : context_(context) {
    InitializeBufferPools(pool_size_per_category);
  }
  
  std::unique_ptr<UcxBuffer> AllocateBuffer(size_t required_size) {
    // Find appropriate size category
    size_t buffer_size = FindOptimalBufferSize(required_size);
    
    // Try to get from pool first
    if (auto buffer = TryGetFromPool(buffer_size)) {
      return buffer;
    }
    
    // Allocate new if pool exhausted
    return AllocateNewBuffer(buffer_size);
  }
  
private:
  void InitializeBufferPools(size_t pool_size_per_category) {
    for (size_t buffer_size : BUFFER_SIZES) {
      for (size_t i = 0; i < pool_size_per_category; ++i) {
        auto buffer_info = AllocateAndRegisterBuffer(buffer_size);
        buffer_pool_.push_back(buffer_info);
      }
    }
  }
  
  BufferInfo AllocateAndRegisterBuffer(size_t size) {
    ucp_mem_map_params_t params{};
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    params.length = size;
    
    ucp_mem_h memory_handle;
    ucs_status_t status = ucp_mem_map(context_, &params, &memory_handle);
    if (status != UCS_OK) {
      throw std::runtime_error("Failed to map UCX memory: " + std::string(ucs_status_string(status)));
    }
    
    ucp_mem_attr_t attr{};
    attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS;
    ucp_mem_query(memory_handle, &attr);
    
    return BufferInfo{
      .data = attr.address,
      .size = size,
      .memory_handle = memory_handle,
      .in_use = false
    };  
  }
  
  std::unique_ptr<UcxBuffer> TryGetFromPool(size_t required_size) {
    // Lock-free pool allocation using atomic operations
    for (size_t attempts = 0; attempts < buffer_pool_.size(); ++attempts) {
      size_t index = next_buffer_index_.fetch_add(1) % buffer_pool_.size();
      BufferInfo& buffer_info = buffer_pool_[index];
      
      if (buffer_info.size >= required_size) {
        bool expected = false;
        if (buffer_info.in_use.compare_exchange_weak(expected, true)) {
          return std::make_unique<PooledUcxBuffer>(&buffer_info);
        }
      }
    }
    return nullptr;  // Pool exhausted
  }
};
```

### **Task 5.3: Optimal Buffer Size Strategy**
**Priority**: MEDIUM - Memory efficiency optimization  
**Location**: Buffer allocation strategy

#### Smart Buffer Sizing
```cpp
class BufferSizeOptimizer {
  struct SizeStats {
    size_t total_requests = 0;
    size_t total_bytes = 0;
    double avg_size() const { return total_requests > 0 ? double(total_bytes) / total_requests : 0; }
  };
  
  absl::flat_hash_map<size_t, SizeStats> size_category_stats_;
  std::atomic<size_t> total_allocations_{0};
  
public:
  size_t FindOptimalBufferSize(size_t required_size) {
    // Record allocation pattern
    RecordAllocation(required_size);
    
    // Find smallest buffer size that fits
    for (size_t pool_size : UcxMemoryPool::BUFFER_SIZES) {
      if (pool_size >= required_size) {
        return pool_size;
      }
    }
    
    // Round up to next power of 2 for very large allocations
    return NextPowerOfTwo(required_size);
  }
  
  void RecordAllocation(size_t size) {
    size_t category = FindSizeCategory(size);
    size_category_stats_[category].total_requests++;
    size_category_stats_[category].total_bytes += size;
    total_allocations_.fetch_add(1);
  }
  
  // Periodically analyze allocation patterns and adjust pool sizes
  void OptimizePoolSizes() {
    if (total_allocations_.load() % 10000 == 0) {
      AnalyzeAndAdjustPoolSizes();
    }
  }
};
```

## Week 6: Connection Optimization

### **Task 6.1: Connection Pooling Implementation**
**Priority**: HIGH - Scale beyond single endpoint limits  
**Location**: Enhanced `ClientWorkerContext`

#### Current Single Endpoint Limitation
```cpp
struct ClientWorkerContext {
  ucp_ep_h endpoint;  // ← Single endpoint limits bandwidth to ~1-2 GB/s
};
```

#### Multi-Endpoint Connection Pool
```cpp
struct ClientWorkerContext {
  std::vector<ucp_ep_h> endpoint_pool_;
  std::atomic<size_t> next_endpoint_index_{0};
  std::vector<AdaptiveFlowControl> per_endpoint_flow_control_;
  
  static constexpr size_t DEFAULT_ENDPOINTS_PER_WORKER = 4;
  
  void InitializeEndpointPool(const std::string& server_addr, size_t pool_size) {
    endpoint_pool_.reserve(pool_size);
    per_endpoint_flow_control_.resize(pool_size);
    
    for (size_t i = 0; i < pool_size; ++i) {
      ucp_ep_h endpoint = CreateEndpoint(server_addr);
      endpoint_pool_.push_back(endpoint);
    }
  }
  
  ucp_ep_h SelectEndpoint() {
    // Load balance across endpoints based on outstanding operations
    size_t best_endpoint = 0;
    size_t min_outstanding = per_endpoint_flow_control_[0].outstanding_operations.load();
    
    for (size_t i = 1; i < endpoint_pool_.size(); ++i) {
      size_t outstanding = per_endpoint_flow_control_[i].outstanding_operations.load();
      if (outstanding < min_outstanding) {
        min_outstanding = outstanding;
        best_endpoint = i;
      }
    }
    
    return endpoint_pool_[best_endpoint];
  }
  
  AdaptiveFlowControl* GetFlowControl(ucp_ep_h endpoint) {
    // Find corresponding flow control for endpoint
    for (size_t i = 0; i < endpoint_pool_.size(); ++i) {
      if (endpoint_pool_[i] == endpoint) {
        return &per_endpoint_flow_control_[i];
      }
    }
    return nullptr;
  }
};
```

#### Connection Health Monitoring
```cpp
class ConnectionHealthMonitor {
  struct EndpointHealth {
    absl::Time last_successful_operation;
    std::atomic<size_t> consecutive_failures{0};
    std::atomic<bool> healthy{true};
  };
  
  absl::flat_hash_map<ucp_ep_h, EndpointHealth> endpoint_health_;
  std::thread monitoring_thread_;
  
public:
  void RecordSuccess(ucp_ep_h endpoint) {
    if (auto it = endpoint_health_.find(endpoint); it != endpoint_health_.end()) {
      it->second.last_successful_operation = absl::Now();
      it->second.consecutive_failures.store(0);
      it->second.healthy.store(true);
    }
  }
  
  void RecordFailure(ucp_ep_h endpoint) {
    if (auto it = endpoint_health_.find(endpoint); it != endpoint_health_.end()) {
      size_t failures = it->second.consecutive_failures.fetch_add(1) + 1;
      if (failures >= 3) {
        it->second.healthy.store(false);
        // Schedule endpoint recreation
        ScheduleEndpointRecreation(endpoint);
      }
    }
  }
  
  bool IsHealthy(ucp_ep_h endpoint) const {
    if (auto it = endpoint_health_.find(endpoint); it != endpoint_health_.end()) {
      return it->second.healthy.load();
    }
    return false;
  }
};
```

### **Task 6.2: Advanced Load Balancing**
**Priority**: MEDIUM - Optimize endpoint utilization  
**Location**: Enhanced endpoint selection

#### Intelligent Endpoint Selection
```cpp
class LoadBalancingStrategy {
public:
  virtual ucp_ep_h SelectEndpoint(const std::vector<ucp_ep_h>& endpoints,
                                 const std::vector<AdaptiveFlowControl>& flow_controls,
                                 const ConnectionHealthMonitor& health_monitor) = 0;
};

class WeightedRoundRobinStrategy : public LoadBalancingStrategy {
  std::atomic<size_t> next_index_{0};
  
public:
  ucp_ep_h SelectEndpoint(const std::vector<ucp_ep_h>& endpoints,
                         const std::vector<AdaptiveFlowControl>& flow_controls,
                         const ConnectionHealthMonitor& health_monitor) override {
    
    // Find healthy endpoints with capacity
    std::vector<std::pair<size_t, double>> candidates;
    
    for (size_t i = 0; i < endpoints.size(); ++i) {
      if (!health_monitor.IsHealthy(endpoints[i])) continue;
      
      size_t outstanding = flow_controls[i].outstanding_operations.load();
      size_t capacity = flow_controls[i].max_outstanding.load();
      
      if (outstanding < capacity) {
        double weight = 1.0 - (double(outstanding) / capacity);  // Higher weight = more capacity
        candidates.emplace_back(i, weight);
      }
    }
    
    if (candidates.empty()) {
      // All endpoints busy or unhealthy - use round robin as fallback
      size_t index = next_index_.fetch_add(1) % endpoints.size();
      return endpoints[index];
    }
    
    // Select based on weighted capacity
    std::sort(candidates.begin(), candidates.end(), 
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    return endpoints[candidates[0].first];
  }
};
```

## Phase 2 Success Metrics

### **Performance Targets**
- **Throughput**: 1-5 GB/s (Conservative) to 7-70 GB/s (Optimistic)
- **Latency**: <10μs per write operation (fire-and-forget completion)
- **Memory Efficiency**: 67% reduction in memory bandwidth usage
- **Concurrency**: Unlimited parallel operations with immediate completion

### **Feature Completeness**
- [ ] Fire-and-forget writes with background reliability tracking
- [ ] Zero-copy memory management with UCX buffer pools
- [ ] Connection pooling with health monitoring
- [ ] Advanced flow control and load balancing
- [ ] Memory registration overhead elimination

### **Validation Tests**
```cpp
TEST(Phase2ValidationTest, FireAndForgetPerformance) {
  // Validate immediate completion
  auto start = absl::Now();
  std::vector<Future<TimestampedStorageGeneration>> futures;
  
  for (int i = 0; i < 1000; ++i) {
    futures.push_back(kvstore::Write(store, absl::StrCat("key", i), 
                                    absl::Cord(std::string(16*1024, 'x'))));
  }
  
  auto completion_time = absl::Now() - start;
  EXPECT_LT(completion_time, absl::Milliseconds(100));  // Should complete in <100ms
  
  // Validate all operations completed immediately
  for (auto& f : futures) {
    EXPECT_TRUE(f.ready());  // All should be immediately ready
    TENSORSTORE_ASSERT_OK(f.result());
  }
}

TEST(Phase2ValidationTest, ZeroCopyMemoryUsage) {
  // Measure memory bandwidth usage
  auto memory_usage_before = GetMemoryBandwidthUsage();
  
  auto future = kvstore::Write(store, "test_key", absl::Cord(std::string(16*1024, 'x')));
  TENSORSTORE_ASSERT_OK(future.result());
  
  auto memory_usage_after = GetMemoryBandwidthUsage();
  auto bandwidth_used = memory_usage_after - memory_usage_before;
  
  // Should use ~16KB, not 48KB (67% reduction from triple copying)
  EXPECT_LT(bandwidth_used, 20*1024);  // Allow some overhead
}

TEST(Phase2ValidationTest, ConnectionPooling) {
  // Validate multiple endpoints are utilized
  auto stats = GetEndpointUtilizationStats();
  EXPECT_GT(stats.active_endpoints, 1);  // Multiple endpoints active
  EXPECT_GT(stats.total_throughput, 1e9); // >1 GB/s aggregate throughput
}
```

Phase 2 achieves the critical performance breakthrough by eliminating RTT dependencies and memory copying overhead, enabling true DRAM-to-DRAM performance levels while maintaining data reliability through background tracking.