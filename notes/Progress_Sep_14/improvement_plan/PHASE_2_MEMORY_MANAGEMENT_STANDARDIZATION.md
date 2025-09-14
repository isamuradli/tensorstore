# Phase 2: Memory Management Standardization
## Priority: HIGH - Eliminates complexity and improves performance

### Overview
Standardize the mixed memory management approach currently used in the implementation. Currently, the code uses UCX native buffers for some operations and regular `new char[]` for others, creating complexity and potential performance issues.

### Current State Analysis
**Mixed Memory Strategy Issues:**
- **Server responses:** Use `UcxNativeBuffer` (lines 320, 396) ✅ Good
- **Client requests:** Use `new char[]` (lines 2109, 2158) ❌ Inconsistent
- **Message building:** Use `new char[]` (line 903) ❌ Inconsistent
- **Complex cleanup logic:** Multiple memory deallocation patterns
- **Performance penalty:** Non-registered memory may be slower for UCX transfers

### Goals
1. ✅ Unified memory allocation strategy using UCX registered memory
2. ✅ Simplified error handling and cleanup logic
3. ✅ Improved performance through consistent zero-copy operations
4. ✅ Maintainable codebase with single memory management pattern
5. ✅ Preserve existing API compatibility

### Phase 2 Prerequisites
- ✅ Phase 1 (Worker Progress Fix) must be completed and stable
- ✅ All Phase 1 tests passing in production for 1 week minimum
- ✅ No critical issues from Phase 1 deployment

### Implementation Steps

#### Step 2.1: Create Unified Buffer Management System
**New File:** `unified_buffer_manager.h`

```cpp
#pragma once
#include "remote_dram_kvstore.h"
#include <memory>
#include <optional>

namespace tensorstore {

class UnifiedBufferManager {
public:
  // Buffer types for different use cases
  enum class BufferType {
    SEND_MESSAGE,       // For outgoing messages (client->server)
    RECEIVE_RESPONSE,   // For incoming responses (server->client)
    SERVER_RESPONSE,    // For server response construction
    ADAPTIVE_RECEIVE    // For server adaptive receive buffers
  };

  // Smart buffer wrapper that handles UCX memory automatically
  class ManagedBuffer {
  public:
    explicit ManagedBuffer(std::unique_ptr<UcxNativeBuffer> buffer)
        : ucx_buffer_(std::move(buffer)) {}

    // Non-copyable, movable
    ManagedBuffer(const ManagedBuffer&) = delete;
    ManagedBuffer& operator=(const ManagedBuffer&) = delete;
    ManagedBuffer(ManagedBuffer&&) = default;
    ManagedBuffer& operator=(ManagedBuffer&&) = default;

    char* data() { return ucx_buffer_ ? ucx_buffer_->data() : nullptr; }
    const char* data() const { return ucx_buffer_ ? ucx_buffer_->data() : nullptr; }
    size_t size() const { return ucx_buffer_ ? ucx_buffer_->size() : 0; }
    bool is_valid() const { return ucx_buffer_ && ucx_buffer_->is_valid(); }
    ucp_mem_h mem_handle() const { return ucx_buffer_ ? ucx_buffer_->mem_handle() : nullptr; }

    // Transfer ownership for zero-copy operations
    std::unique_ptr<UcxNativeBuffer> release() { return std::move(ucx_buffer_); }

  private:
    std::unique_ptr<UcxNativeBuffer> ucx_buffer_;
  };

  // Factory methods for different buffer types
  static std::optional<ManagedBuffer> AllocateForSend(
      ucp_context_h context, size_t size);

  static std::optional<ManagedBuffer> AllocateForReceive(
      ucp_context_h context, size_t size);

  static std::optional<ManagedBuffer> AllocateForServerResponse(
      ucp_context_h context, size_t size);

  static std::optional<ManagedBuffer> AllocateForAdaptiveReceive(
      ucp_context_h context, size_t size);

  // Statistics and monitoring
  struct Statistics {
    std::atomic<uint64_t> total_allocations{0};
    std::atomic<uint64_t> failed_allocations{0};
    std::atomic<uint64_t> bytes_allocated{0};
    std::atomic<size_t> active_buffers{0};
  };

  static Statistics& GetStatistics();

private:
  static std::optional<ManagedBuffer> AllocateInternal(
      ucp_context_h context, size_t size, BufferType type);
};

} // namespace tensorstore
```

#### Step 2.2: Implement Unified Buffer Manager
**New File:** `unified_buffer_manager.cc`

```cpp
#include "unified_buffer_manager.h"
#include "absl/log/absl_log.h"

namespace tensorstore {

std::optional<UnifiedBufferManager::ManagedBuffer>
UnifiedBufferManager::AllocateForSend(ucp_context_h context, size_t size) {
  return AllocateInternal(context, size, BufferType::SEND_MESSAGE);
}

std::optional<UnifiedBufferManager::ManagedBuffer>
UnifiedBufferManager::AllocateForReceive(ucp_context_h context, size_t size) {
  return AllocateInternal(context, size, BufferType::RECEIVE_RESPONSE);
}

std::optional<UnifiedBufferManager::ManagedBuffer>
UnifiedBufferManager::AllocateForServerResponse(ucp_context_h context, size_t size) {
  return AllocateInternal(context, size, BufferType::SERVER_RESPONSE);
}

std::optional<UnifiedBufferManager::ManagedBuffer>
UnifiedBufferManager::AllocateForAdaptiveReceive(ucp_context_h context, size_t size) {
  return AllocateInternal(context, size, BufferType::ADAPTIVE_RECEIVE);
}

std::optional<UnifiedBufferManager::ManagedBuffer>
UnifiedBufferManager::AllocateInternal(ucp_context_h context, size_t size, BufferType type) {
  auto& stats = GetStatistics();
  stats.total_allocations++;

  auto ucx_buffer = UcxNativeBuffer::Create(context, size);
  if (!ucx_buffer || !ucx_buffer->is_valid()) {
    stats.failed_allocations++;
    ABSL_LOG(ERROR) << "Failed to allocate UCX buffer of size " << size
                    << " for type " << static_cast<int>(type);
    return std::nullopt;
  }

  stats.bytes_allocated += size;
  stats.active_buffers++;

  // Log allocation for debugging (can be disabled in production)
  ABSL_LOG(INFO) << "Allocated UCX buffer: size=" << size
                 << ", type=" << static_cast<int>(type)
                 << ", total_active=" << stats.active_buffers.load();

  return ManagedBuffer(std::move(ucx_buffer));
}

UnifiedBufferManager::Statistics& UnifiedBufferManager::GetStatistics() {
  static Statistics stats;
  return stats;
}

} // namespace tensorstore
```

#### Step 2.3: Update ZeroCopyMessageBuilder to Use Unified Buffers
**File:** `remote_dram_kvstore.cc` lines 857-961

**Replace Existing BuildWriteMessage:**
```cpp
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
};
```

#### Step 2.4: Update Client Request Methods
**File:** `remote_dram_kvstore.cc` lines 1857-1973 (WriteRemote)

**Replace Existing WriteRemote Buffer Allocation:**
```cpp
Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key,
                                                 const absl::Cord& value) {
  // ... existing connection setup code ...

  auto& ucx_manager = UcxManager::Instance();
  uint64_t request_id = ucx_manager.GenerateRequestId();

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

  // ... existing promise/future and registration code ...

  // Post write response receive with unified buffer
  PostWriteResponseReceiveUnified(request_id);

  // Send message using UCX tagged messaging with registered memory
  ucp_request_param_t send_params;
  memset(&send_params, 0, sizeof(send_params));
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                            UCP_OP_ATTR_FIELD_USER_DATA |
                            UCP_OP_ATTR_FIELD_MEMH;  // Use registered memory
  send_params.cb.send = SendCallbackUnified;
  send_params.memh = message_result.mem_handle;  // UCX registered memory handle

  // Create user data for callback - now holds ManagedBuffer
  struct SendUserDataUnified {
    uint64_t request_id;
    UnifiedBufferManager::ManagedBuffer buffer;  // RAII cleanup
  };

  auto* user_data = new SendUserDataUnified{
      .request_id = request_id,
      .buffer = std::move(message_result.buffer)
  };
  send_params.user_data = user_data;

  void* request = ucp_tag_send_nbx(client_endpoint_, user_data->buffer.data(),
                                   message_result.total_size,
                                   UCX_TAG_WRITE_REQUEST, &send_params);

  // Handle request status (same pattern as before, but with unified cleanup)
  if (UCS_PTR_IS_ERR(request)) {
    ucs_status_t error_status = UCS_PTR_STATUS(request);
    ABSL_LOG(ERROR) << "UCX send failed: " << ucs_status_string(error_status);
    ucx_manager.CompletePendingOperationDirect(request_id,
        absl::InternalError("UCX send failed"));
    delete user_data;  // ManagedBuffer automatically cleans up UCX memory
  } else if (request == nullptr) {
    // Immediate completion
    ABSL_LOG(INFO) << "Write request sent immediately";
    delete user_data;  // ManagedBuffer automatically cleans up UCX memory
  }
  // For non-NULL requests: SendCallbackUnified will handle cleanup

  // ... existing future transformation code ...
}
```

#### Step 2.5: Update Client Receive Methods
**File:** `remote_dram_kvstore.cc` lines 2102-2149, 2151-2196

**Create Unified Receive Methods:**
```cpp
void PostReadResponseReceiveUnified(uint64_t request_id) {
  auto& ucx_manager = UcxManager::Instance();

  // Use unified buffer allocation for receive
  constexpr size_t max_response_size = 4 * 1024 * 1024 + 1024;  // 4MB + header
  auto buffer_opt = UnifiedBufferManager::AllocateForReceive(
      ucx_manager.GetContext(), max_response_size);

  if (!buffer_opt) {
    ABSL_LOG(ERROR) << "Failed to allocate receive buffer";
    // Complete operation with error
    kvstore::ReadResult error_result;
    error_result.state = kvstore::ReadResult::kMissing;
    error_result.stamp.generation = StorageGeneration::NoValue();
    error_result.stamp.time = absl::Now();
    ucx_manager.CompletePendingReadOperationDirect(request_id, std::move(error_result));
    return;
  }

  // Create unified user data structure
  struct ReadUserDataUnified {
    uint64_t request_id;
    UnifiedBufferManager::ManagedBuffer buffer;
  };

  auto* user_data = new ReadUserDataUnified{
      .request_id = request_id,
      .buffer = std::move(*buffer_opt)
  };

  // Post non-blocking receive with registered memory
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                            UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ClientReceiveCallbackUnified;
  recv_params.user_data = user_data;

  void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(),
                                   user_data->buffer.data(),
                                   max_response_size,
                                   UCX_TAG_READ_RESPONSE, UCX_TAG_MASK,
                                   &recv_params);

  // Handle request status with unified cleanup
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post read receive: "
                    << ucs_status_string(UCS_PTR_STATUS(request));

    kvstore::ReadResult error_result;
    error_result.state = kvstore::ReadResult::kMissing;
    error_result.stamp.generation = StorageGeneration::NoValue();
    error_result.stamp.time = absl::Now();
    ucx_manager.CompletePendingReadOperationDirect(request_id, std::move(error_result));

    delete user_data;  // ManagedBuffer handles UCX memory cleanup
  }
  // For successful requests: ClientReceiveCallbackUnified handles cleanup
}
```

#### Step 2.6: Update Callback Functions
**File:** `remote_dram_kvstore.cc` lines 499-538, 698-792, 794-850

**Create Unified Callbacks:**
```cpp
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

/// Unified client receive callback with automatic UCX memory management
void ClientReceiveCallbackUnified(void* request, ucs_status_t status,
                                 const ucp_tag_recv_info_t* info, void* user_data) {
  struct ReadUserDataUnified {
    uint64_t request_id;
    UnifiedBufferManager::ManagedBuffer buffer;
  };

  ReadUserDataUnified* read_data = static_cast<ReadUserDataUnified*>(user_data);
  uint64_t request_id = read_data->request_id;

  kvstore::ReadResult result;

  if (status == UCS_OK && info->length >= sizeof(ReadResponse)) {
    // Process response using unified buffer
    ReadResponse* response = reinterpret_cast<ReadResponse*>(read_data->buffer.data());

    // ... existing response processing logic ...

    if (response->status_code == 0 && response->header.value_length > 0) {
      // Zero-copy Cord construction with UCX buffer ownership transfer
      const char* value_ptr = read_data->buffer.data() + sizeof(ReadResponse);

      // Transfer UCX buffer ownership to Cord for true zero-copy
      auto owned_buffer = read_data->buffer.release();  // Transfer ownership
      result.value = absl::MakeCordFromExternal(
        absl::string_view(value_ptr, response->header.value_length),
        [buffer = std::move(owned_buffer)](absl::string_view) {
          // UCX buffer will be automatically cleaned up when Cord is destroyed
        });

      result.state = kvstore::ReadResult::kValue;
      result.stamp.generation = StorageGeneration::FromString("remote_read");
      result.stamp.time = absl::Now();
    } else {
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
    }
  } else {
    ABSL_LOG(ERROR) << "Failed to receive read response: " << ucs_status_string(status);
    result.state = kvstore::ReadResult::kMissing;
    result.stamp.generation = StorageGeneration::NoValue();
    result.stamp.time = absl::Now();
  }

  // Complete operation
  UcxManager::Instance().CompletePendingReadOperationDirect(request_id, std::move(result));

  // Cleanup: ManagedBuffer destructor handles UCX memory (unless transferred to Cord)
  delete read_data;

  if (request != nullptr) {
    ucp_request_free(request);
  }
}
```

### Testing Strategy

#### Step 2.7: Memory Management Unit Tests
```cpp
TEST(UnifiedBufferManagerTest, AllocationAndCleanup) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  auto buffer_opt = UnifiedBufferManager::AllocateForSend(
      manager.GetContext(), 1024);
  ASSERT_TRUE(buffer_opt.has_value());

  auto& buffer = *buffer_opt;
  EXPECT_TRUE(buffer.is_valid());
  EXPECT_EQ(buffer.size(), 1024);
  EXPECT_NE(buffer.data(), nullptr);
  EXPECT_NE(buffer.mem_handle(), nullptr);

  // Test automatic cleanup on destruction
  auto& stats = UnifiedBufferManager::GetStatistics();
  auto initial_active = stats.active_buffers.load();
  {
    auto temp_buffer = UnifiedBufferManager::AllocateForReceive(
        manager.GetContext(), 512);
    EXPECT_EQ(stats.active_buffers.load(), initial_active + 1);
  }
  // Buffer should be automatically cleaned up
  EXPECT_EQ(stats.active_buffers.load(), initial_active);

  manager.Shutdown();
}

TEST(UnifiedBufferManagerTest, ZeroCopyMessageBuilding) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  kvstore::Key key = "test_key";
  absl::Cord value("test_value_data");
  uint64_t request_id = 12345;

  auto message_opt = ZeroCopyMessageBuilder::BuildWriteMessage(
      manager.GetContext(), key, value, request_id);

  ASSERT_TRUE(message_opt.has_value());

  auto& message = *message_opt;
  EXPECT_TRUE(message.buffer.is_valid());
  EXPECT_GT(message.total_size, sizeof(MessageHeader) + key.size() + value.size());
  EXPECT_NE(message.mem_handle, nullptr);

  // Verify message content
  MessageHeader* header = reinterpret_cast<MessageHeader*>(message.buffer.data());
  EXPECT_EQ(header->magic_number, MESSAGE_MAGIC_NUMBER);
  EXPECT_EQ(header->type, MessageType::WRITE_REQUEST);
  EXPECT_EQ(header->request_id, request_id);

  manager.Shutdown();
}

TEST(UnifiedBufferManagerTest, MemoryLeakDetection) {
  // Test that all allocated buffers are properly cleaned up
  auto& stats = UnifiedBufferManager::GetStatistics();
  auto initial_active = stats.active_buffers.load();

  {
    UcxManager& manager = UcxManager::Instance();
    ASSERT_OK(manager.Initialize());

    // Allocate many buffers
    std::vector<UnifiedBufferManager::ManagedBuffer> buffers;
    for (int i = 0; i < 100; ++i) {
      auto buffer_opt = UnifiedBufferManager::AllocateForSend(
          manager.GetContext(), 1024);
      if (buffer_opt) {
        buffers.push_back(std::move(*buffer_opt));
      }
    }

    EXPECT_EQ(stats.active_buffers.load(), initial_active + 100);

    manager.Shutdown();
  }

  // All buffers should be cleaned up
  EXPECT_EQ(stats.active_buffers.load(), initial_active);
}
```

#### Performance Validation Tests
```cpp
TEST(UnifiedBufferPerformanceTest, CompareWithOldImplementation) {
  // Benchmark unified vs mixed memory management
  // Expect 10-15% improvement in allocation time
  // Expect reduced memory fragmentation
}

TEST(UnifiedBufferIntegrationTest, FullOperationCycle) {
  // Test complete read/write cycle with unified buffers
  // Verify no memory leaks
  // Verify performance is maintained or improved
}
```

### Validation Criteria

#### Performance Requirements
- [ ] No regression in operation throughput
- [ ] 10-15% improvement in memory allocation efficiency
- [ ] Reduced memory fragmentation
- [ ] Lower peak memory usage

#### Functional Requirements
- [ ] All existing tests pass with unified memory management
- [ ] Zero-copy operations work correctly
- [ ] Proper cleanup in all error scenarios
- [ ] Compatible with Phase 1 improvements

#### Memory Safety Requirements
- [ ] No memory leaks under any scenario
- [ ] Proper UCX memory cleanup
- [ ] No buffer overruns or underruns
- [ ] Safe transfer of ownership to Cord objects

### Rollback Plan
1. **Configuration Flag:** Add `use_unified_memory_` flag to enable/disable
2. **Gradual Migration:** Start with new allocations only, keep existing cleanup
3. **Immediate Rollback:** Revert to mixed approach if issues occur

### Risk Assessment

#### High Risk: Memory Leaks
- **Mitigation:** Extensive leak testing with valgrind
- **Detection:** Memory usage monitoring
- **Response:** Immediate rollback if leaks detected

#### Medium Risk: Performance Regression
- **Mitigation:** Comprehensive benchmarking
- **Detection:** Performance monitoring
- **Response:** Optimization or rollback

#### Low Risk: UCX Compatibility
- **Mitigation:** Test with multiple UCX versions
- **Detection:** Unit test failures
- **Response:** Compatibility fixes

### Success Metrics
- [ ] Unified memory management implemented across all operations
- [ ] Performance maintained or improved
- [ ] Memory usage optimized
- [ ] Codebase complexity reduced
- [ ] Foundation ready for Phase 3

### Estimated Timeline
- **Development:** 5 days
- **Testing:** 3 days
- **Integration:** 2 days
- **Total:** 2 weeks

### Next Phase Prerequisites
Phase 2 must demonstrate:
- Zero memory leaks for 1 week in production
- Performance metrics stable or improved
- All integration tests passing
- Reduced code complexity metrics