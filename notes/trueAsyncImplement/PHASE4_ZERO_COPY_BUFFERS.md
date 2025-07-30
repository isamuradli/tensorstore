# Phase 4: Zero-Copy Buffer Management

## Problem Analysis
**Current Issue**: Triple buffer copying wastes memory bandwidth and CPU cache
- **Copy 1**: `absl::Cord` → `std::string` (line 1672)
- **Copy 2**: Data → `std::vector<char>` message buffer (line 1665)
- **Copy 3**: Message buffer → stable UCX buffer (line 1716)
- **Impact**: 3x memory bandwidth overhead + CPU cache pollution

## Current Triple-Copy Pattern
```cpp
Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key, const absl::Cord& value) {
  // COPY 1: Cord to string
  std::string value_str = std::string(value);  // 16KB copy
  
  // COPY 2: Build message buffer
  size_t message_size = sizeof(MessageHeader) + key.size() + value.size();
  std::vector<char> message_buffer(message_size);  // 16KB allocation
  std::memcpy(data_ptr + key.size(), value_str.data(), value_str.size());  // 16KB copy
  
  // COPY 3: Create stable buffer for UCX
  char* stable_buffer = new char[message_size];  // 16KB allocation
  std::memcpy(stable_buffer, message_buffer.data(), message_size);  // 16KB copy
  
  // Total: 48KB copied for 16KB of actual data!
}
```

## Phase 4 Solution: Zero-Copy Buffer Architecture

### Goals
1. **Eliminate all intermediate copies**
2. **Direct UCX buffer construction**
3. **Memory pool management** for buffer reuse
4. **Maintain network isolation** (no shared memory)

### Architecture: Direct Buffer Construction

#### Step 4.1: UCX Memory Pool
```cpp
class UcxMemoryPool {
private:
  struct PooledBuffer {
    char* data;
    size_t size;
    std::atomic<bool> in_use{false};
    
    PooledBuffer(size_t sz) : size(sz), data(new char[sz]) {}
    ~PooledBuffer() { delete[] data; }
  };
  
  std::vector<std::unique_ptr<PooledBuffer>> small_buffers_;   // 1KB buffers
  std::vector<std::unique_ptr<PooledBuffer>> medium_buffers_;  // 16KB buffers
  std::vector<std::unique_ptr<PooledBuffer>> large_buffers_;   // 64KB buffers
  
public:
  struct BufferLease {
    char* data;
    size_t size;
    PooledBuffer* source_buffer;
    
    ~BufferLease() {
      if (source_buffer) {
        source_buffer->in_use.store(false);  // Return to pool
      }
    }
  };
  
  std::unique_ptr<BufferLease> AcquireBuffer(size_t required_size) {
    // Select appropriate pool based on size
    auto& pool = SelectPool(required_size);
    
    // Find available buffer
    for (auto& buffer : pool) {
      bool expected = false;
      if (buffer->in_use.compare_exchange_weak(expected, true)) {
        return std::make_unique<BufferLease>(BufferLease{
          buffer->data, buffer->size, buffer.get()
        });
      }
    }
    
    // No available buffer - create new one
    auto new_buffer = std::make_unique<PooledBuffer>(required_size);
    new_buffer->in_use.store(true);
    BufferLease* lease = new BufferLease{new_buffer->data, new_buffer->size, new_buffer.get()};
    pool.push_back(std::move(new_buffer));
    return std::unique_ptr<BufferLease>(lease);
  }
};
```

#### Step 4.2: Zero-Copy Message Construction
```cpp
class ZeroCopyMessageBuilder {
public:
  struct MessageBuffer {
    std::unique_ptr<UcxMemoryPool::BufferLease> buffer_lease;
    MessageHeader* header;
    char* payload_start;
    size_t total_size;
  };
  
  static std::unique_ptr<MessageBuffer> BuildWriteMessage(
      const kvstore::Key& key, const absl::Cord& value, uint64_t request_id) {
    
    size_t total_size = sizeof(MessageHeader) + key.size() + value.size();
    
    // Get buffer from pool (NO ALLOCATION if available)
    auto buffer_lease = UcxMemoryPool::Instance().AcquireBuffer(total_size);
    
    // Construct message directly in UCX buffer (NO COPIES)
    char* buffer = buffer_lease->data;
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
    char* payload = buffer + sizeof(MessageHeader);
    
    // Initialize header directly
    message_utils::InitializeMessageHeader(
        header, MessageType::WRITE_REQUEST,
        static_cast<uint32_t>(key.size()),
        static_cast<uint32_t>(value.size()),
        request_id, payload);
    
    // Copy key directly to final location (SINGLE COPY)
    std::memcpy(payload, key.data(), key.size());
    
    // Copy value directly to final location (SINGLE COPY)
    value.CopyToArray(payload + key.size());  // absl::Cord zero-copy when possible
    
    return std::make_unique<MessageBuffer>(MessageBuffer{
        std::move(buffer_lease), header, payload, total_size
    });
  }
};
```

#### Step 4.3: Zero-Copy Write Operation
```cpp
Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key, const absl::Cord& value) {
  auto* worker_ctx = ucx_manager.SelectWorkerForWrite(key);
  uint64_t request_id = worker_ctx->GenerateRequestId();
  
  // Build message directly in UCX buffer (NO INTERMEDIATE COPIES)
  auto message = ZeroCopyMessageBuilder::BuildWriteMessage(key, value, request_id);
  
  // Send directly from constructed buffer
  ucp_request_param_t send_params;
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  send_params.cb.send = ZeroCopyWriteCallback;
  send_params.user_data = message.release();  // Transfer ownership to callback
  
  void* request = ucp_tag_send_nbx(
      worker_ctx->endpoint,
      message->buffer_lease->data,     // Direct buffer - no copies
      message->total_size,
      UCX_TAG_WRITE_REQUEST,
      &send_params);
  
  // Handle UCX send result...
  
  // Return immediately (fire-and-forget from Phase 3)
  TimestampedStorageGeneration result;
  result.generation = StorageGeneration::FromString("zero_copy_write");
  result.time = absl::Now();
  return MakeReadyFuture<TimestampedStorageGeneration>(result);
}
```

#### Step 4.4: Zero-Copy Callback Cleanup
```cpp
void ZeroCopyWriteCallback(void* request, ucs_status_t status, void* user_data) {
  // user_data contains MessageBuffer with buffer lease
  auto* message = static_cast<ZeroCopyMessageBuilder::MessageBuffer*>(user_data);
  
  if (status == UCS_OK) {
    ABSL_LOG(INFO) << "Zero-copy write completed successfully";
  } else {
    ABSL_LOG(ERROR) << "Zero-copy write failed: " << ucs_status_string(status);
  }
  
  // Buffer automatically returns to pool when MessageBuffer destructor runs
  delete message;
  ucp_request_free(request);
}
```

### Advanced: absl::Cord Zero-Copy Integration

#### Leverage absl::Cord's Zero-Copy Features
```cpp
class CordZeroCopyBuilder {
public:
  static std::unique_ptr<MessageBuffer> BuildFromCord(
      const kvstore::Key& key, const absl::Cord& value, uint64_t request_id) {
    
    // Check if Cord is flat (single contiguous buffer)
    if (value.size() <= 4096 && value.TryFlat()) {
      // Can reference Cord's internal buffer directly
      return BuildWithCordReference(key, value, request_id);
    } else {
      // Fall back to single-copy approach
      return BuildWithSingleCopy(key, value, request_id);
    }
  }
  
private:
  static std::unique_ptr<MessageBuffer> BuildWithCordReference(
      const kvstore::Key& key, const absl::Cord& value, uint64_t request_id) {
    
    // Get Cord's internal buffer
    absl::string_view flat_value = value.Flatten();
    
    // Build message with header + key, reference value directly
    size_t header_size = sizeof(MessageHeader) + key.size();
    auto buffer_lease = UcxMemoryPool::Instance().AcquireBuffer(header_size);
    
    // Construct header and key
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer_lease->data);
    char* key_ptr = buffer_lease->data + sizeof(MessageHeader);
    std::memcpy(key_ptr, key.data(), key.size());
    
    // Initialize header with separate value buffer
    message_utils::InitializeMessageHeader(
        header, MessageType::WRITE_REQUEST,
        static_cast<uint32_t>(key.size()),
        static_cast<uint32_t>(value.size()),
        request_id, flat_value.data());  // Reference Cord's buffer
    
    // Use UCX scatter-gather for multiple buffers
    return BuildScatterGatherMessage(std::move(buffer_lease), flat_value);
  }
};
```

### Memory Usage Optimization

#### Before Phase 4: Memory Waste
```
For 16KB write:
- Cord internal buffer: 16KB
- String copy: 16KB
- Message buffer: 16KB + header
- Stable buffer: 16KB + header
Total: ~64KB for 16KB of data
```

#### After Phase 4: Memory Efficiency
```
For 16KB write:
- Cord internal buffer: 16KB
- UCX buffer (from pool): 16KB + header
Total: ~32KB for 16KB of data (50% reduction)
```

### Implementation Steps

#### Step 4.1: UCX Memory Pool
1. **Create buffer pool** with size classes
2. **Implement buffer leasing** with automatic return
3. **Add pool statistics** and monitoring

#### Step 4.2: Zero-Copy Message Builder
1. **Direct buffer construction** in UCX memory
2. **Single-copy key and value** placement
3. **Eliminate intermediate buffers**

#### Step 4.3: Integration with Phases 1-3
1. **Combine with fire-and-forget** writes (Phase 3)
2. **Use with multiple workers** (Phase 2)
3. **Maintain high-frequency polling** (Phase 1)

#### Step 4.4: Advanced Optimizations
1. **absl::Cord zero-copy** when possible
2. **UCX scatter-gather** for multiple buffers
3. **Memory-mapped regions** for large transfers

### Expected Performance Impact

#### Memory Bandwidth
- **50% reduction** in memory copies
- **Better CPU cache utilization**
- **Reduced memory allocation pressure**

#### CPU Performance
- **Lower CPU usage** for large writes
- **Better cache locality**
- **Reduced GC pressure** (fewer allocations)

#### Your Use Case Impact
```cpp
// 100 concurrent 16KB writes:
// Before: 100 × 48KB = 4.8MB of copying
// After:  100 × 16KB = 1.6MB of copying (67% reduction)
```

### Configuration Options
```cpp
struct ZeroCopyConfig {
  size_t small_buffer_size = 1024;      // 1KB buffers
  size_t medium_buffer_size = 16384;    // 16KB buffers  
  size_t large_buffer_size = 65536;     // 64KB buffers
  
  size_t small_pool_size = 100;         // Number of small buffers
  size_t medium_pool_size = 50;         // Number of medium buffers
  size_t large_pool_size = 20;          // Number of large buffers
  
  bool enable_cord_zero_copy = true;     // Use Cord's internal buffers
  bool enable_scatter_gather = true;     // UCX multi-buffer sends
};
```

### Questions for User
1. **Pool Sizes**: What buffer pool sizes work for your use case?
2. **Memory vs Speed**: Trade more memory (larger pools) for fewer allocations?
3. **Cord Integration**: Should we use absl::Cord's zero-copy features when possible?

### Integration with Previous Phases
This phase **enhances** the benefits from Phases 1-3:
- ✅ **Phase 1**: High-frequency polling handles zero-copy operations efficiently
- ✅ **Phase 2**: Multiple workers benefit from reduced memory pressure
- ✅ **Phase 3**: Fire-and-forget writes with zero-copy achieve maximum throughput
- ✅ **Your goal**: chunk1 writes with minimal CPU and memory overhead

## Files to Modify
- New file: `tensorstore/kvstore/remote_dram/ucx_memory_pool.h`
- New file: `tensorstore/kvstore/remote_dram/zero_copy_builder.h`
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:1650-1773` (WriteRemote)
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:373-407` (callbacks)

## Testing Strategy
- Measure memory usage reduction
- Test buffer pool efficiency
- Verify zero-copy path correctness
- Benchmark CPU usage improvement