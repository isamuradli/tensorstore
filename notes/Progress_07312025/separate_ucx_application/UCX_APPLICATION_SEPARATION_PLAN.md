# Separation of Concerns Implementation Plan - UCX Network Layer vs Application Layer

## Executive Summary

This plan addresses the architectural violation where UCX (network layer) is inappropriately handling application logic. The current implementation has UCX callbacks performing message parsing, storage operations, promise management, and protocol handling - responsibilities that should belong to the application layer.

**Goal**: Create clean separation between network communication (UCX) and application logic (Remote DRAM KVStore) to improve performance, maintainability, and scalability.

**Expected Benefits**:
- 10-20x reduction in UCX callback latency (500μs → 25μs)
- Elimination of thread contention between network and application work
- Improved error isolation and system reliability
- Better scalability through proper architectural layering

---

## Current Architecture Problems

### Problem Analysis

```cpp
// CURRENT PROBLEMATIC ARCHITECTURE:
UCX Callback → Application Logic → Storage Operations → Promise Resolution
     ↓              ↓                    ↓                    ↓
  Network         Message            Key-Value            Async
 Completion       Parsing             Storage           Coordination
```

**Issues**:
1. **Single Responsibility Violation**: UCX handles both network and application concerns
2. **Tight Coupling**: Network callbacks directly call application functions
3. **Thread Safety Issues**: Application work blocks UCX progress thread
4. **Error Propagation**: Application errors can crash network layer
5. **Performance Degradation**: Long-running application logic in network callbacks

---

## Target Architecture

### Clean Separation Model

```cpp
// TARGET CLEAN ARCHITECTURE:

┌─────────────────────────────────────────────────────────────┐
│                    APPLICATION LAYER                        │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────┐ │
│  │ Message Handler │  │ Storage Manager │  │ Async Coord │ │
│  │ - Parse messages│  │ - Key-value ops │  │ - Promises  │ │
│  │ - Validate data │  │ - DRAM storage  │  │ - Futures   │ │
│  │ - Generate resp │  │ - Cleanup       │  │ - Timeouts  │ │
│  └─────────────────┘  └─────────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              ↑ ↓
                    ┌─────────────────────────┐
                    │    EVENT BRIDGE         │
                    │ - Event conversion      │
                    │ - Minimal coordination  │
                    │ - Error translation     │
                    └─────────────────────────┘
                              ↑ ↓
┌─────────────────────────────────────────────────────────────┐
│                     NETWORK LAYER (UCX)                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────┐ │
│  │ Connection Mgmt │  │ Data Transfer   │  │ Progress    │ │
│  │ - Endpoints     │  │ - Send/receive  │  │ - Polling   │ │
│  │ - Listeners     │  │ - Memory reg    │  │ - Events    │ │
│  │ - Health check  │  │ - Zero-copy     │  │ - Callbacks │ │
│  └─────────────────┘  └─────────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## PHASE 1: Network Event Abstraction (Week 1-2)

### Goal
Create clean network event abstraction that separates UCX completions from application logic.

### 1.1 Network Event System

```cpp
// NEW: Clean network event abstraction
namespace network {

enum class EventType {
    DATA_RECEIVED,
    DATA_SENT,
    CONNECTION_ESTABLISHED,
    CONNECTION_LOST,
    ERROR_OCCURRED
};

struct NetworkEvent {
    EventType type;
    ucp_ep_h endpoint;
    void* buffer;
    size_t buffer_size;
    size_t bytes_transferred;
    ucs_status_t status;
    uint64_t operation_id;
    std::chrono::steady_clock::time_point timestamp;
    
    // User data from original operation
    void* user_context;
};

// Network event handler interface
class NetworkEventHandler {
public:
    virtual ~NetworkEventHandler() = default;
    virtual void HandleNetworkEvent(const NetworkEvent& event) = 0;
};

}  // namespace network
```

### 1.2 Pure UCX Network Layer

```cpp
// NEW: Pure network communication layer
class UcxNetworkLayer {
public:
    // ✅ ONLY network responsibilities
    struct SendOperation {
        uint64_t operation_id;
        ucp_ep_h endpoint;
        const void* data;
        size_t size;
        ucp_tag_t tag;
        void* user_context;
    };
    
    struct ReceiveOperation {
        uint64_t operation_id;
        void* buffer;
        size_t buffer_size;
        ucp_tag_t tag;
        ucp_tag_t tag_mask;
        void* user_context;
    };
    
    // Pure network interface - no application logic
    absl::Status SendData(const SendOperation& op);
    absl::Status ReceiveData(const ReceiveOperation& op);
    absl::Status CreateConnection(const std::string& address, ucp_ep_h* endpoint);
    absl::Status StartListener(const std::string& address, ucp_listener_h* listener);
    
    // Event handler registration
    void SetEventHandler(network::NetworkEventHandler* handler) {
        event_handler_ = handler;
    }
    
private:
    network::NetworkEventHandler* event_handler_ = nullptr;
    
    // ✅ CLEAN UCX callbacks - ONLY network work
    static void PureSendCallback(void* request, ucs_status_t status, void* user_data);
    static void PureReceiveCallback(void* request, ucs_status_t status, 
                                   const ucp_tag_recv_info_t* info, void* user_data);
    
    // ✅ Minimal event notification
    void NotifyNetworkEvent(const network::NetworkEvent& event) {
        if (event_handler_) {
            event_handler_->HandleNetworkEvent(event);
        }
    }
};
```

### 1.3 Clean UCX Callbacks

```cpp
// NEW: Pure network callbacks - NO application logic
void UcxNetworkLayer::PureSendCallback(void* request, ucs_status_t status, void* user_data) {
    auto* context = static_cast<SendCallbackContext*>(user_data);
    
    // ✅ ONLY network completion notification
    network::NetworkEvent event{};
    event.type = network::EventType::DATA_SENT;
    event.endpoint = context->endpoint;
    event.status = status;
    event.operation_id = context->operation_id;
    event.bytes_transferred = context->size;
    event.timestamp = std::chrono::steady_clock::now();
    event.user_context = context->user_context;
    
    // ✅ ONLY notify application layer
    context->network_layer->NotifyNetworkEvent(event);
    
    // ✅ ONLY cleanup network resources
    delete context;
    ucp_request_free(request);
}

void UcxNetworkLayer::PureReceiveCallback(void* request, ucs_status_t status, 
                                         const ucp_tag_recv_info_t* info, void* user_data) {
    auto* context = static_cast<ReceiveCallbackContext*>(user_data);
    
    // ✅ ONLY network completion notification
    network::NetworkEvent event{};
    event.type = network::EventType::DATA_RECEIVED;
    event.endpoint = context->endpoint;
    event.buffer = context->buffer;
    event.buffer_size = context->buffer_size;
    event.bytes_transferred = info ? info->length : 0;
    event.status = status;
    event.operation_id = context->operation_id;
    event.timestamp = std::chrono::steady_clock::now();
    event.user_context = context->user_context;
    
    // ✅ ONLY notify application layer
    context->network_layer->NotifyNetworkEvent(event);
    
    // ✅ ONLY cleanup network resources
    delete context;
    ucp_request_free(request);
}
```

### Implementation Steps - Phase 1
1. **Week 1**: Implement network event system and pure UCX layer
2. **Week 1-2**: Replace existing UCX callbacks with pure network callbacks
3. **Week 2**: Add comprehensive unit tests for network layer isolation
4. **Week 2**: Performance validation - measure callback latency reduction

### Phase 1 Testing and Validation

#### 1.1 Remote DRAM KVStore Functional Tests
```cpp
// NEW: Comprehensive functional tests for Phase 1
class Phase1RemoteDramFunctionalTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize separated network layer
        network_layer_ = std::make_unique<UcxNetworkLayer>();
        ASSERT_TRUE(network_layer_->Initialize().ok());
        
        // Set up test server and client
        server_endpoint_ = SetupTestServer("127.0.0.1:12345");
        client_endpoint_ = SetupTestClient("127.0.0.1:12345");
    }
    
    void TearDown() override {
        network_layer_->Shutdown();
    }
    
private:
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    ucp_ep_h server_endpoint_;
    ucp_ep_h client_endpoint_;
};

// Test basic write operation works with new network layer
TEST_F(Phase1RemoteDramFunctionalTests, BasicWriteOperation) {
    // Create test key-value pair
    std::string test_key = "test_key_phase1";
    std::string test_value = "test_value_data_phase1";
    
    // Create write request message
    auto message_buffer = CreateWriteRequestMessage(test_key, test_value, 12345);
    
    // Send via new network layer
    network::SendOperation send_op{};
    send_op.operation_id = 1;
    send_op.endpoint = server_endpoint_;
    send_op.data = message_buffer.data();
    send_op.size = message_buffer.size();
    send_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto status = network_layer_->SendData(send_op);
    EXPECT_TRUE(status.ok()) << "Write send failed: " << status;
    
    // Wait for network completion
    auto completion_status = WaitForNetworkCompletion(send_op.operation_id, 5000);
    EXPECT_TRUE(completion_status.ok()) << "Write completion failed: " << completion_status;
    
    // Verify network event notification was received
    EXPECT_TRUE(HasReceivedNetworkEvent(network::EventType::DATA_SENT, send_op.operation_id));
}

// Test basic read operation works with new network layer
TEST_F(Phase1RemoteDramFunctionalTests, BasicReadOperation) {
    // Pre-populate server with test data
    std::string test_key = "read_test_key_phase1";
    std::string test_value = "read_test_value_phase1";
    PrePopulateServerData(test_key, test_value);
    
    // Create read request message
    auto message_buffer = CreateReadRequestMessage(test_key, 12346);
    
    // Send via new network layer
    network::SendOperation send_op{};
    send_op.operation_id = 2;
    send_op.endpoint = server_endpoint_;
    send_op.data = message_buffer.data();
    send_op.size = message_buffer.size();
    send_op.tag = UCX_TAG_READ_REQUEST;
    
    auto status = network_layer_->SendData(send_op);
    EXPECT_TRUE(status.ok()) << "Read send failed: " << status;
    
    // Wait for response
    auto response = WaitForReadResponse(12346, 5000);
    EXPECT_TRUE(response.ok()) << "Read response failed: " << response.status();
    EXPECT_EQ(response.value().value, test_value);
}

// Test multiple concurrent operations
TEST_F(Phase1RemoteDramFunctionalTests, ConcurrentOperations) {
    constexpr int kNumOperations = 10;
    std::vector<std::future<absl::Status>> operation_futures;
    
    // Launch multiple concurrent write operations
    for (int i = 0; i < kNumOperations; ++i) {
        auto future = std::async(std::launch::async, [this, i]() {
            std::string key = absl::StrFormat("concurrent_key_%d", i);
            std::string value = absl::StrFormat("concurrent_value_%d", i);
            
            auto message = CreateWriteRequestMessage(key, value, 20000 + i);
            
            network::SendOperation op{};
            op.operation_id = 20000 + i;
            op.endpoint = server_endpoint_;
            op.data = message.data();
            op.size = message.size();
            op.tag = UCX_TAG_WRITE_REQUEST;
            
            return network_layer_->SendData(op);
        });
        
        operation_futures.push_back(std::move(future));
    }
    
    // Wait for all operations to complete
    for (auto& future : operation_futures) {
        auto status = future.get();
        EXPECT_TRUE(status.ok()) << "Concurrent operation failed: " << status;
    }
    
    // Verify all network events were properly handled
    for (int i = 0; i < kNumOperations; ++i) {
        EXPECT_TRUE(HasReceivedNetworkEvent(network::EventType::DATA_SENT, 20000 + i));
    }
}

// Test error handling in network layer
TEST_F(Phase1RemoteDramFunctionalTests, NetworkErrorHandling) {
    // Test with invalid endpoint
    network::SendOperation invalid_op{};
    invalid_op.operation_id = 99999;
    invalid_op.endpoint = nullptr;  // Invalid endpoint
    invalid_op.data = "test_data";
    invalid_op.size = 9;
    invalid_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto status = network_layer_->SendData(invalid_op);
    EXPECT_FALSE(status.ok()) << "Should fail with invalid endpoint";
    
    // Test with oversized message
    std::string large_data(10 * 1024 * 1024, 'x');  // 10MB
    network::SendOperation large_op{};
    large_op.operation_id = 99998;
    large_op.endpoint = server_endpoint_;
    large_op.data = large_data.data();
    large_op.size = large_data.size();
    large_op.tag = UCX_TAG_WRITE_REQUEST;
    
    // Should either succeed or fail gracefully
    auto large_status = network_layer_->SendData(large_op);
    if (!large_status.ok()) {
        // If it fails, should be a proper error, not a crash
        EXPECT_THAT(large_status.message(), testing::HasSubstr("too large"));
    }
}
```

#### 1.2 Network Layer Isolation Tests
```cpp
// Test network callback latency improvement
TEST_F(Phase1RemoteDramFunctionalTests, CallbackLatencyImprovement) {
    constexpr int kNumMeasurements = 100;
    std::vector<std::chrono::nanoseconds> callback_latencies;
    
    for (int i = 0; i < kNumMeasurements; ++i) {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Trigger network callback
        TriggerNetworkCallback(i);
        
        // Wait for callback completion
        WaitForCallbackCompletion(i);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        callback_latencies.push_back(end_time - start_time);
    }
    
    // Calculate average latency
    auto total_latency = std::accumulate(callback_latencies.begin(), 
                                       callback_latencies.end(), 
                                       std::chrono::nanoseconds(0));
    auto avg_latency = total_latency / kNumMeasurements;
    
    // Expect significant improvement (target <25μs)
    EXPECT_LT(avg_latency, std::chrono::microseconds(25))
        << "Average callback latency: " << avg_latency.count() << "ns";
    
    // 95th percentile should also be low
    std::sort(callback_latencies.begin(), callback_latencies.end());
    auto p95_latency = callback_latencies[static_cast<size_t>(kNumMeasurements * 0.95)];
    EXPECT_LT(p95_latency, std::chrono::microseconds(50))
        << "95th percentile latency: " << p95_latency.count() << "ns";
}

// Test that network callbacks don't perform application logic
TEST_F(Phase1RemoteDramFunctionalTests, CallbackPurityTest) {
    NetworkCallbackTracker tracker;
    network_layer_->SetCallbackTracker(&tracker);
    
    // Send test message
    auto message = CreateWriteRequestMessage("purity_test", "data", 30000);
    network::SendOperation op{};
    op.operation_id = 30000;
    op.endpoint = server_endpoint_;
    op.data = message.data();
    op.size = message.size();
    op.tag = UCX_TAG_WRITE_REQUEST;
    
    network_layer_->SendData(op);
    WaitForNetworkCompletion(30000, 5000);
    
    // Verify callback only did network work
    EXPECT_FALSE(tracker.PerformedApplicationWork())
        << "Network callback performed application logic";
    EXPECT_TRUE(tracker.OnlyPerformedNetworkWork())
        << "Network callback should only do network cleanup";
    EXPECT_LT(tracker.GetCallbackDuration(), std::chrono::microseconds(25))
        << "Network callback took too long: " << tracker.GetCallbackDuration().count() << "μs";
}
```

#### 1.3 Backward Compatibility Tests
```cpp
// Test that existing TensorStore API still works
TEST_F(Phase1RemoteDramFunctionalTests, TensorStoreAPICompatibility) {
    // Create RemoteDramDriver with new network layer
    auto driver_spec = RemoteDramDriverSpec::Make("127.0.0.1:12345");
    ASSERT_TRUE(driver_spec.ok());
    
    auto driver = RemoteDramDriver::Create(driver_spec.value());
    ASSERT_TRUE(driver.ok());
    
    // Test basic TensorStore write operation
    kvstore::Key test_key = "tensorstore_api_test";
    absl::Cord test_value("tensorstore_test_data");
    
    auto write_future = driver.value()->Write(test_key, test_value, {});
    auto write_result = write_future.result();
    EXPECT_TRUE(write_result.ok()) << "TensorStore write failed: " << write_result.status();
    
    // Test basic TensorStore read operation
    auto read_future = driver.value()->Read(test_key, {});
    auto read_result = read_future.result();
    EXPECT_TRUE(read_result.ok()) << "TensorStore read failed: " << read_result.status();
    EXPECT_EQ(read_result.value().value, test_value);
}

// Test that performance characteristics are maintained or improved
TEST_F(Phase1RemoteDramFunctionalTests, PerformanceRegression) {
    constexpr int kNumOperations = 1000;
    constexpr size_t kValueSize = 1024;  // 1KB values
    
    std::string test_value(kValueSize, 'x');
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Perform batch operations
    std::vector<std::future<absl::Status>> futures;
    for (int i = 0; i < kNumOperations; ++i) {
        std::string key = absl::StrFormat("perf_test_%d", i);
        
        auto future = std::async(std::launch::async, [this, key, test_value, i]() {
            auto message = CreateWriteRequestMessage(key, test_value, 40000 + i);
            
            network::SendOperation op{};
            op.operation_id = 40000 + i;
            op.endpoint = server_endpoint_;
            op.data = message.data();
            op.size = message.size();
            op.tag = UCX_TAG_WRITE_REQUEST;
            
            return network_layer_->SendData(op);
        });
        
        futures.push_back(std::move(future));
    }
    
    // Wait for completion
    for (auto& future : futures) {
        auto status = future.get();
        EXPECT_TRUE(status.ok());
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate throughput
    double throughput_mbps = (kNumOperations * kValueSize) / (1024.0 * 1024.0) / 
                           (total_time.count() / 1000.0);
    
    // Expect at least 50 MB/s (baseline performance)
    EXPECT_GE(throughput_mbps, 50.0) 
        << "Throughput regression: " << throughput_mbps << " MB/s";
    
    ABSL_LOG(INFO) << "Phase 1 throughput: " << throughput_mbps << " MB/s";
}
```

#### Phase 1 Success Criteria
- [ ] **Functional Correctness**: All basic read/write operations work with new network layer
- [ ] **Callback Latency**: Average callback latency < 25μs (10x improvement target)
- [ ] **Callback Purity**: Network callbacks perform zero application logic
- [ ] **API Compatibility**: Existing TensorStore API continues to work unchanged
- [ ] **Performance Baseline**: No throughput regression below 50 MB/s
- [ ] **Concurrency**: 10+ concurrent operations complete successfully
- [ ] **Error Handling**: Network errors are properly isolated and reported

---

## PHASE 2: Application Message Handler (Week 3-4)

### Goal
Create dedicated application layer that handles all business logic separate from network.

### 2.1 Application Message Handler

```cpp
// NEW: Pure application logic layer
namespace application {

class RemoteDramMessageHandler : public network::NetworkEventHandler {
public:
    RemoteDramMessageHandler(RemoteDramStorage* storage, 
                           AsyncOperationManager* async_mgr)
        : storage_(storage), async_manager_(async_mgr) {}
    
    // ✅ Application handles its own network events
    void HandleNetworkEvent(const network::NetworkEvent& event) override {
        switch (event.type) {
            case network::EventType::DATA_RECEIVED:
                HandleReceivedData(event);
                break;
            case network::EventType::DATA_SENT:
                HandleSentData(event);
                break;
            case network::EventType::CONNECTION_ESTABLISHED:
                HandleNewConnection(event);
                break;
            case network::EventType::CONNECTION_LOST:
                HandleLostConnection(event);
                break;
            case network::EventType::ERROR_OCCURRED:
                HandleNetworkError(event);
                break;
        }
    }
    
private:
    RemoteDramStorage* storage_;
    AsyncOperationManager* async_manager_;
    UcxNetworkLayer* network_layer_;
    
    void HandleReceivedData(const network::NetworkEvent& event) {
        // ✅ Application validates its own messages
        auto parse_result = ParseMessage(event.buffer, event.bytes_transferred);
        if (!parse_result.ok()) {
            ABSL_LOG(ERROR) << "Message parsing failed: " << parse_result.status();
            return;
        }
        
        const auto& message = parse_result.value();
        
        // ✅ Application handles its own protocols
        switch (message.type) {
            case MessageType::WRITE_REQUEST:
                HandleWriteRequest(message, event.endpoint);
                break;
            case MessageType::READ_REQUEST:
                HandleReadRequest(message, event.endpoint);
                break;
            case MessageType::WRITE_RESPONSE:
                HandleWriteResponse(message);
                break;
            case MessageType::READ_RESPONSE:
                HandleReadResponse(message);
                break;
        }
        
        // ✅ Application manages its own buffer cleanup
        ReleaseApplicationBuffer(event.buffer);
    }
    
    void HandleWriteRequest(const ParsedMessage& message, ucp_ep_h endpoint) {
        // ✅ Application performs its own storage operations
        storage_->Store(message.key, message.value);
        
        // ✅ Application generates its own responses
        auto response = CreateWriteResponse(message.request_id, 0);  // success
        
        // ✅ Application requests network send (clean interface)
        network::SendOperation send_op{};
        send_op.operation_id = GenerateOperationId();
        send_op.endpoint = endpoint;
        send_op.data = response.data();
        send_op.size = response.size();
        send_op.tag = UCX_TAG_WRITE_RESPONSE;
        send_op.user_context = CreateResponseContext(message.request_id);
        
        network_layer_->SendData(send_op);
    }
    
    void HandleReadRequest(const ParsedMessage& message, ucp_ep_h endpoint) {
        // ✅ Application performs its own storage lookups
        auto value = storage_->Get(message.key);
        
        // ✅ Application generates its own responses
        auto response = CreateReadResponse(message.request_id, value);
        
        // ✅ Clean network interface usage
        network::SendOperation send_op{};
        send_op.operation_id = GenerateOperationId();
        send_op.endpoint = endpoint;
        send_op.data = response.data();
        send_op.size = response.size();
        send_op.tag = UCX_TAG_READ_RESPONSE;
        send_op.user_context = CreateResponseContext(message.request_id);
        
        network_layer_->SendData(send_op);
    }
    
    void HandleWriteResponse(const ParsedMessage& message) {
        // ✅ Application manages its own async operations
        auto operation = async_manager_->FindPendingOperation(message.request_id);
        if (operation) {
            absl::Status result = (message.status_code == 0) ? 
                absl::OkStatus() : absl::InternalError("Write failed");
            operation->promise.SetResult(result);
        }
    }
    
    void HandleReadResponse(const ParsedMessage& message) {
        // ✅ Application manages its own async operations
        auto operation = async_manager_->FindPendingReadOperation(message.request_id);
        if (operation) {
            kvstore::ReadResult result = CreateReadResult(message);
            operation->promise.SetResult(std::move(result));
        }
    }
};

}  // namespace application
```

### 2.2 Message Parsing and Validation

```cpp
// NEW: Application-owned message handling
namespace application {

struct ParsedMessage {
    MessageType type;
    uint64_t request_id;
    std::string key;
    absl::Cord value;
    uint32_t status_code;
    bool is_valid;
};

class MessageParser {
public:
    static Result<ParsedMessage> ParseMessage(const void* buffer, size_t size) {
        // ✅ Application validates its own message format
        if (size < sizeof(MessageHeader)) {
            return absl::InvalidArgumentError("Message too small");
        }
        
        const MessageHeader* header = static_cast<const MessageHeader*>(buffer);
        
        // ✅ Application validates its own protocol
        if (header->magic_number != MESSAGE_MAGIC_NUMBER) {
            return absl::InvalidArgumentError("Invalid magic number");
        }
        
        // ✅ Application handles its own integrity checking
        if (!ValidateMessageIntegrity(header, size)) {
            return absl::InvalidArgumentError("Message integrity check failed");
        }
        
        ParsedMessage message{};
        message.type = header->type;
        message.request_id = header->request_id;
        message.is_valid = true;
        
        // ✅ Application parses its own message content
        const char* payload = static_cast<const char*>(buffer) + sizeof(MessageHeader);
        
        if (header->key_length > 0) {
            message.key = std::string(payload, header->key_length);
            payload += header->key_length;
        }
        
        if (header->value_length > 0) {
            message.value = absl::Cord(std::string(payload, header->value_length));
        }
        
        return message;
    }
    
private:
    static bool ValidateMessageIntegrity(const MessageHeader* header, size_t total_size) {
        // ✅ Application-specific validation logic
        size_t expected_size = sizeof(MessageHeader) + header->key_length + header->value_length;
        if (total_size < expected_size) return false;
        
        if (header->checksum != 0) {
            const void* payload = reinterpret_cast<const char*>(header) + sizeof(MessageHeader);
            uint32_t calculated = CalculateChecksum(payload, header->key_length + header->value_length);
            return calculated == header->checksum;
        }
        
        return true;
    }
};

}  // namespace application
```

### Implementation Steps - Phase 2
1. **Week 3**: Implement application message handler and parser
2. **Week 3**: Create application-layer async operation management
3. **Week 4**: Integrate application handler with network layer
4. **Week 4**: Unit tests for application logic separation

### Phase 2 Testing and Validation

#### 2.1 Application Layer Functional Tests
```cpp
// NEW: Comprehensive application layer tests for Phase 2
class Phase2ApplicationLayerTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize separated layers
        network_layer_ = std::make_unique<UcxNetworkLayer>();
        storage_ = std::make_unique<RemoteDramStorage>();
        async_manager_ = std::make_unique<AsyncOperationManager>();
        
        // Create application message handler
        app_handler_ = std::make_unique<application::RemoteDramMessageHandler>(
            storage_.get(), async_manager_.get());
        
        // Set up network-application integration
        network_layer_->SetEventHandler(app_handler_.get());
        
        ASSERT_TRUE(network_layer_->Initialize().ok());
        
        // Set up test server and client
        server_endpoint_ = SetupTestServer("127.0.0.1:12346");
        client_endpoint_ = SetupTestClient("127.0.0.1:12346");
    }
    
    void TearDown() override {
        network_layer_->Shutdown();
    }
    
private:
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    std::unique_ptr<RemoteDramStorage> storage_;
    std::unique_ptr<AsyncOperationManager> async_manager_;
    std::unique_ptr<application::RemoteDramMessageHandler> app_handler_;
    ucp_ep_h server_endpoint_;
    ucp_ep_h client_endpoint_;
};

// Test complete write flow through separated layers
TEST_F(Phase2ApplicationLayerTests, CompleteWriteFlow) {
    std::string test_key = "app_layer_write_test";
    std::string test_value = "application_layer_test_data";
    uint64_t request_id = 50001;
    
    // Create write request message
    auto message = CreateWriteRequestMessage(test_key, test_value, request_id);
    
    // Send through network layer
    network::SendOperation send_op{};
    send_op.operation_id = request_id;
    send_op.endpoint = server_endpoint_;
    send_op.data = message.data();
    send_op.size = message.size();
    send_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto status = network_layer_->SendData(send_op);
    EXPECT_TRUE(status.ok()) << "Network send failed: " << status;
    
    // Wait for application layer to process the request
    WaitForApplicationProcessing(request_id, 5000);
    
    // Verify data was stored by application layer
    auto stored_value = storage_->Get(test_key);
    EXPECT_TRUE(stored_value.has_value()) << "Data not stored by application layer";
    EXPECT_EQ(std::string(*stored_value), test_value);
    
    // Verify write response was sent back
    auto response = WaitForWriteResponse(request_id, 5000);
    EXPECT_TRUE(response.ok()) << "Write response failed: " << response.status();
    EXPECT_EQ(response.value().status_code, 0);  // Success
}

// Test complete read flow through separated layers
TEST_F(Phase2ApplicationLayerTests, CompleteReadFlow) {
    std::string test_key = "app_layer_read_test";
    std::string test_value = "application_read_test_data";
    uint64_t request_id = 50002;
    
    // Pre-populate storage through application layer
    storage_->Store(test_key, absl::Cord(test_value));
    
    // Create read request message
    auto message = CreateReadRequestMessage(test_key, request_id);
    
    // Send through network layer
    network::SendOperation send_op{};
    send_op.operation_id = request_id;
    send_op.endpoint = server_endpoint_;
    send_op.data = message.data();
    send_op.size = message.size();
    send_op.tag = UCX_TAG_READ_REQUEST;
    
    auto status = network_layer_->SendData(send_op);
    EXPECT_TRUE(status.ok()) << "Network send failed: " << status;
    
    // Wait for application layer to process and respond
    auto response = WaitForReadResponse(request_id, 5000);
    EXPECT_TRUE(response.ok()) << "Read response failed: " << response.status();
    EXPECT_EQ(response.value().status_code, 0);  // Success
    EXPECT_EQ(response.value().value, test_value);
}

// Test message parsing and validation in application layer
TEST_F(Phase2ApplicationLayerTests, MessageParsingValidation) {
    // Test valid message parsing
    std::string valid_key = "valid_test_key";
    std::string valid_value = "valid_test_value";
    auto valid_message = CreateWriteRequestMessage(valid_key, valid_value, 50003);
    
    auto parse_result = application::MessageParser::ParseMessage(
        valid_message.data(), valid_message.size());
    EXPECT_TRUE(parse_result.ok()) << "Valid message parsing failed: " << parse_result.status();
    
    const auto& parsed = parse_result.value();
    EXPECT_EQ(parsed.type, MessageType::WRITE_REQUEST);
    EXPECT_EQ(parsed.key, valid_key);
    EXPECT_EQ(std::string(parsed.value), valid_value);
    EXPECT_EQ(parsed.request_id, 50003);
    EXPECT_TRUE(parsed.is_valid);
    
    // Test invalid message handling
    std::vector<char> invalid_message(10, 'x');  // Too small, invalid format
    auto invalid_parse_result = application::MessageParser::ParseMessage(
        invalid_message.data(), invalid_message.size());
    EXPECT_FALSE(invalid_parse_result.ok()) << "Invalid message should fail parsing";
    
    // Test corrupted message handling
    auto corrupted_message = CreateWriteRequestMessage("key", "value", 50004);
    corrupted_message[sizeof(MessageHeader) / 2] = 0xFF;  // Corrupt the message
    
    auto corrupt_parse_result = application::MessageParser::ParseMessage(
        corrupted_message.data(), corrupted_message.size());
    // Should either fail parsing or detect corruption
    if (corrupt_parse_result.ok()) {
        EXPECT_FALSE(corrupt_parse_result.value().is_valid) 
            << "Corrupted message should be detected as invalid";
    }
}

// Test async operation management in application layer
TEST_F(Phase2ApplicationLayerTests, AsyncOperationManagement) {
    constexpr int kNumAsyncOps = 20;
    std::vector<uint64_t> request_ids;
    std::vector<Promise<kvstore::ReadResult>> promises;
    
    // Register multiple pending read operations
    for (int i = 0; i < kNumAsyncOps; ++i) {
        uint64_t request_id = 60000 + i;
        auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
        
        async_manager_->RegisterReadOperation(request_id, std::move(promise));
        request_ids.push_back(request_id);
        promises.push_back(std::move(promise));
    }
    
    // Verify operations are tracked
    for (uint64_t request_id : request_ids) {
        EXPECT_TRUE(async_manager_->HasPendingReadOperation(request_id))
            << "Request ID " << request_id << " not tracked";
    }
    
    // Complete operations through application layer
    for (int i = 0; i < kNumAsyncOps; ++i) {
        uint64_t request_id = request_ids[i];
        kvstore::ReadResult result;
        result.state = kvstore::ReadResult::kValue;
        result.value = absl::Cord(absl::StrFormat("test_value_%d", i));
        result.stamp.generation = StorageGeneration::FromString("test_gen");
        result.stamp.time = absl::Now();
        
        async_manager_->CompletePendingReadOperation(request_id, std::move(result));
    }
    
    // Verify all operations completed
    for (uint64_t request_id : request_ids) {
        EXPECT_FALSE(async_manager_->HasPendingReadOperation(request_id))
            << "Request ID " << request_id << " still tracked after completion";
    }
}

// Test storage isolation in application layer
TEST_F(Phase2ApplicationLayerTests, StorageIsolation) {
    // Test that application storage is independent of network layer
    std::map<std::string, std::string> test_data = {
        {"isolation_key_1", "isolation_value_1"},
        {"isolation_key_2", "isolation_value_2"},
        {"isolation_key_3", "isolation_value_3"}
    };
    
    // Store data through application layer
    for (const auto& [key, value] : test_data) {
        storage_->Store(key, absl::Cord(value));
    }
    
    // Verify data through application layer
    for (const auto& [key, expected_value] : test_data) {
        auto stored_value = storage_->Get(key);
        EXPECT_TRUE(stored_value.has_value()) << "Key " << key << " not found";
        EXPECT_EQ(std::string(*stored_value), expected_value);
    }
    
    // Verify storage statistics
    EXPECT_EQ(storage_->GetKeyCount(), test_data.size());
    
    auto all_keys = storage_->GetAllKeys();
    EXPECT_EQ(all_keys.size(), test_data.size());
    
    for (const auto& [key, value] : test_data) {
        EXPECT_NE(std::find(all_keys.begin(), all_keys.end(), key), all_keys.end())
            << "Key " << key << " not in key list";
    }
}
```

#### 2.2 Layer Separation Tests
```cpp
// Test that application logic is properly separated from network callbacks
TEST_F(Phase2ApplicationLayerTests, LayerSeparationValidation) {
    NetworkCallbackTracker network_tracker;
    ApplicationEventTracker app_tracker;
    
    network_layer_->SetCallbackTracker(&network_tracker);
    app_handler_->SetEventTracker(&app_tracker);
    
    // Send test message that will trigger both network and application processing
    std::string test_key = "separation_test";
    std::string test_value = "separation_data";
    auto message = CreateWriteRequestMessage(test_key, test_value, 50005);
    
    network::SendOperation send_op{};
    send_op.operation_id = 50005;
    send_op.endpoint = server_endpoint_;
    send_op.data = message.data();
    send_op.size = message.size();
    send_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    network_layer_->SendData(send_op);
    
    // Wait for both network and application processing
    WaitForNetworkCompletion(50005, 5000);
    WaitForApplicationProcessing(50005, 5000);
    auto end_time = std::chrono::high_resolution_clock::now();
    
    // Verify network callback was fast and pure
    EXPECT_LT(network_tracker.GetCallbackDuration(), std::chrono::microseconds(25))
        << "Network callback too slow";
    EXPECT_FALSE(network_tracker.PerformedApplicationWork())
        << "Network callback performed application work";
    EXPECT_TRUE(network_tracker.OnlyPerformedNetworkWork())
        << "Network callback did non-network work";
    
    // Verify application processing happened separately
    EXPECT_TRUE(app_tracker.ProcessedMessage()) << "Application didn't process message";
    EXPECT_TRUE(app_tracker.PerformedStorageOperation()) << "Application didn't store data";
    EXPECT_TRUE(app_tracker.GeneratedResponse()) << "Application didn't generate response";
    
    // Verify total latency is reasonable despite separation
    auto total_latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    EXPECT_LT(total_latency, std::chrono::milliseconds(10))
        << "Total operation latency too high: " << total_latency.count() << "μs";
    
    // Verify data was actually stored
    auto stored_value = storage_->Get(test_key);
    EXPECT_TRUE(stored_value.has_value());
    EXPECT_EQ(std::string(*stored_value), test_value);
}

// Test error isolation between layers
TEST_F(Phase2ApplicationLayerTests, ErrorIsolation) {
    // Test that application errors don't crash network layer
    ApplicationErrorInjector error_injector;
    app_handler_->SetErrorInjector(&error_injector);
    
    // Inject application-level error
    error_injector.InjectParsingError();
    
    // Send message that will trigger the error
    auto message = CreateWriteRequestMessage("error_test", "data", 50006);
    network::SendOperation send_op{};
    send_op.operation_id = 50006;
    send_op.endpoint = server_endpoint_;
    send_op.data = message.data();
    send_op.size = message.size();
    send_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto status = network_layer_->SendData(send_op);
    EXPECT_TRUE(status.ok()) << "Network layer failed due to application error";
    
    // Network layer should complete successfully
    WaitForNetworkCompletion(50006, 5000);
    
    // Network layer should still be functional
    EXPECT_TRUE(network_layer_->IsHealthy()) << "Network layer unhealthy after app error";
    EXPECT_TRUE(CanCreateNewConnections()) << "Can't create connections after app error";
    
    // Application error should be contained
    EXPECT_FALSE(storage_->Exists("error_test")) << "Data stored despite application error";
    
    // Clear error injection and verify normal operation resumes
    error_injector.ClearErrors();
    
    auto normal_message = CreateWriteRequestMessage("normal_after_error", "data", 50007);
    network::SendOperation normal_op{};
    normal_op.operation_id = 50007;
    normal_op.endpoint = server_endpoint_;
    normal_op.data = normal_message.data();
    normal_op.size = normal_message.size();
    normal_op.tag = UCX_TAG_WRITE_REQUEST;
    
    auto normal_status = network_layer_->SendData(normal_op);
    EXPECT_TRUE(normal_status.ok()) << "Normal operation failed after error recovery";
    
    WaitForApplicationProcessing(50007, 5000);
    EXPECT_TRUE(storage_->Exists("normal_after_error")) << "Normal operation didn't work after error";
}
```

#### 2.3 Integration and Performance Tests
```cpp
// Test end-to-end performance with separated layers
TEST_F(Phase2ApplicationLayerTests, EndToEndPerformance) {
    constexpr int kNumOperations = 500;
    constexpr size_t kValueSize = 2048;  // 2KB values
    
    std::string test_value(kValueSize, 'x');
    std::vector<std::future<absl::Status>> write_futures;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Launch concurrent write operations
    for (int i = 0; i < kNumOperations; ++i) {
        auto future = std::async(std::launch::async, [this, i, test_value]() {
            std::string key = absl::StrFormat("perf_phase2_%d", i);
            uint64_t request_id = 70000 + i;
            
            auto message = CreateWriteRequestMessage(key, test_value, request_id);
            
            network::SendOperation op{};
            op.operation_id = request_id;
            op.endpoint = server_endpoint_;
            op.data = message.data();
            op.size = message.size();
            op.tag = UCX_TAG_WRITE_REQUEST;
            
            auto send_status = network_layer_->SendData(op);
            if (!send_status.ok()) return send_status;
            
            // Wait for complete processing (network + application)
            return WaitForCompleteProcessing(request_id, 10000);
        });
        
        write_futures.push_back(std::move(future));
    }
    
    // Wait for all operations to complete
    int successful_operations = 0;
    for (auto& future : write_futures) {
        auto status = future.get();
        if (status.ok()) {
            successful_operations++;
        } else {
            ABSL_LOG(ERROR) << "Operation failed: " << status;
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate throughput
    double throughput_mbps = (successful_operations * kValueSize) / (1024.0 * 1024.0) / 
                           (total_time.count() / 1000.0);
    
    // Expect at least 40 MB/s (accounting for separation overhead)
    EXPECT_GE(throughput_mbps, 40.0) 
        << "Throughput with separated layers: " << throughput_mbps << " MB/s";
    EXPECT_GE(successful_operations, kNumOperations * 0.95)  // 95% success rate
        << "Success rate: " << (100.0 * successful_operations / kNumOperations) << "%";
    
    // Verify all successful operations were stored
    int stored_count = 0;
    for (int i = 0; i < kNumOperations; ++i) {
        std::string key = absl::StrFormat("perf_phase2_%d", i);
        if (storage_->Exists(key)) {
            stored_count++;
        }
    }
    EXPECT_EQ(stored_count, successful_operations) << "Storage count mismatch";
    
    ABSL_LOG(INFO) << "Phase 2 throughput: " << throughput_mbps << " MB/s"
                   << ", Success rate: " << (100.0 * successful_operations / kNumOperations) << "%"
                   << ", Stored: " << stored_count << "/" << kNumOperations;
}

// Test memory usage and cleanup
TEST_F(Phase2ApplicationLayerTests, MemoryManagement) {
    size_t initial_memory = GetCurrentMemoryUsage();
    
    constexpr int kNumOperations = 100;
    
    // Perform operations that should be cleaned up
    for (int i = 0; i < kNumOperations; ++i) {
        std::string key = absl::StrFormat("memory_test_%d", i);
        std::string value(1024, 'M');  // 1KB value
        
        auto message = CreateWriteRequestMessage(key, value, 80000 + i);
        
        network::SendOperation op{};
        op.operation_id = 80000 + i;
        op.endpoint = server_endpoint_;
        op.data = message.data();
        op.size = message.size();
        op.tag = UCX_TAG_WRITE_REQUEST;
        
        network_layer_->SendData(op);
        WaitForCompleteProcessing(80000 + i, 5000);
    }
    
    // Force cleanup
    network_layer_->CleanupResources();
    app_handler_->CleanupResources();
    
    size_t final_memory = GetCurrentMemoryUsage();
    size_t memory_increase = final_memory - initial_memory;
    
    // Memory increase should be reasonable (stored data + some overhead)
    size_t expected_increase = kNumOperations * 1024;  // Stored data
    size_t max_acceptable_increase = expected_increase * 2;  // 100% overhead max
    
    EXPECT_LT(memory_increase, max_acceptable_increase)
        << "Memory usage increased by " << memory_increase << " bytes, expected < " 
        << max_acceptable_increase;
        
    ABSL_LOG(INFO) << "Phase 2 memory increase: " << memory_increase << " bytes"
                   << " (expected: " << expected_increase << ")";
}
```

#### Phase 2 Success Criteria
- [ ] **Complete Separation**: Application logic completely separated from network callbacks
- [ ] **Functional Correctness**: All read/write operations work through separated layers
- [ ] **Message Processing**: Application correctly parses and validates all message types
- [ ] **Storage Operations**: Data correctly stored and retrieved through application layer
- [ ] **Async Management**: Promise/future operations properly handled by application layer
- [ ] **Error Isolation**: Application errors don't affect network layer functionality
- [ ] **Performance**: < 20% throughput degradation compared to Phase 1 (≥40 MB/s)
- [ ] **Memory Efficiency**: < 100% memory overhead for layer separation
- [ ] **Layer Purity**: Network callbacks perform zero application work

---

## PHASE 3: Clean Integration Layer (Week 5-6)

### Goal
Create minimal integration layer that coordinates between network and application without mixing concerns.

### 3.1 Event Bridge

```cpp
// NEW: Minimal coordination between network and application layers
class NetworkApplicationBridge {
public:
    NetworkApplicationBridge(UcxNetworkLayer* network_layer,
                           application::RemoteDramMessageHandler* app_handler)
        : network_layer_(network_layer), app_handler_(app_handler) {
        
        // ✅ Clean registration of application handler
        network_layer_->SetEventHandler(app_handler_);
    }
    
    // ✅ Application requests network operations through clean interface
    absl::Status SendMessage(ucp_ep_h endpoint, const void* data, size_t size, 
                            ucp_tag_t tag, void* user_context) {
        network::SendOperation op{};
        op.operation_id = GenerateOperationId();
        op.endpoint = endpoint;
        op.data = data;
        op.size = size;
        op.tag = tag;
        op.user_context = user_context;
        
        return network_layer_->SendData(op);
    }
    
    absl::Status PostReceive(void* buffer, size_t buffer_size, 
                           ucp_tag_t tag, ucp_tag_t tag_mask, void* user_context) {
        network::ReceiveOperation op{};
        op.operation_id = GenerateOperationId();
        op.buffer = buffer;
        op.buffer_size = buffer_size;
        op.tag = tag;
        op.tag_mask = tag_mask;
        op.user_context = user_context;
        
        return network_layer_->ReceiveData(op);
    }
    
    // ✅ Clean connection management interface
    absl::Status CreateConnection(const std::string& address, ucp_ep_h* endpoint) {
        return network_layer_->CreateConnection(address, endpoint);
    }
    
    absl::Status StartServer(const std::string& listen_address) {
        ucp_listener_h listener;
        return network_layer_->StartListener(listen_address, &listener);
    }
    
private:
    UcxNetworkLayer* network_layer_;
    application::RemoteDramMessageHandler* app_handler_;
    std::atomic<uint64_t> next_operation_id_{1};
    
    uint64_t GenerateOperationId() {
        return next_operation_id_.fetch_add(1, std::memory_order_relaxed);
    }
};
```

### 3.2 Updated RemoteDramDriver

```cpp
// NEW: Clean driver implementation using separated layers
class RemoteDramDriver : public internal::KvStoreDriver {
public:
    Future<kvstore::ReadResult> Read(Key key, ReadOptions options) override {
        // ✅ Application layer handles its own async coordination
        auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
        
        // ✅ Application creates its own messages
        auto message = CreateReadRequestMessage(key, GenerateRequestId());
        
        // ✅ Clean network interface usage
        auto status = bridge_->SendMessage(
            GetServerEndpoint(),
            message.data(),
            message.size(),
            UCX_TAG_READ_REQUEST,
            CreateReadContext(promise, key)
        );
        
        if (!status.ok()) {
            promise.SetResult(status);
        }
        
        return std::move(future);
    }
    
    Future<TimestampedStorageGeneration> Write(Key key, 
                                             std::optional<Value> value,
                                             WriteOptions options) override {
        // ✅ Application layer handles its own async coordination
        auto [promise, future] = PromiseFuturePair<TimestampedStorageGeneration>::Make();
        
        // ✅ Application creates its own messages
        auto message = CreateWriteRequestMessage(key, value, GenerateRequestId());
        
        // ✅ Clean network interface usage
        auto status = bridge_->SendMessage(
            GetServerEndpoint(),
            message.data(),
            message.size(),
            UCX_TAG_WRITE_REQUEST,
            CreateWriteContext(promise, key)
        );
        
        if (!status.ok()) {
            TimestampedStorageGeneration result;
            result.generation = StorageGeneration::Unknown();
            result.time = absl::Now();
            promise.SetResult(result);
        }
        
        return std::move(future);
    }
    
private:
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    std::unique_ptr<application::RemoteDramMessageHandler> app_handler_;
    std::unique_ptr<NetworkApplicationBridge> bridge_;
    RemoteDramStorage storage_;  // ✅ Application owns its storage
    AsyncOperationManager async_manager_;  // ✅ Application owns its async ops
};
```

### Implementation Steps - Phase 3
1. **Week 5**: Implement network-application bridge
2. **Week 5**: Update RemoteDramDriver to use clean separation
3. **Week 6**: Integration testing and performance validation
4. **Week 6**: Documentation and code cleanup

### Phase 3 Testing and Validation

#### 3.1 Integration Bridge Functional Tests
```cpp
// NEW: Comprehensive integration bridge tests for Phase 3
class Phase3IntegrationBridgeTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize complete separated architecture
        network_layer_ = std::make_unique<UcxNetworkLayer>();
        ASSERT_TRUE(network_layer_->Initialize().ok());
        
        storage_ = std::make_unique<RemoteDramStorage>();
        async_manager_ = std::make_unique<AsyncOperationManager>();
        
        app_handler_ = std::make_unique<application::RemoteDramMessageHandler>(
            storage_.get(), async_manager_.get());
        
        // Create integration bridge
        bridge_ = std::make_unique<NetworkApplicationBridge>(
            network_layer_.get(), app_handler_.get());
        
        // Set up endpoints
        server_endpoint_ = SetupTestServer("127.0.0.1:12347");
        client_endpoint_ = SetupTestClient("127.0.0.1:12347");
    }
    
    void TearDown() override {
        bridge_.reset();
        network_layer_->Shutdown();
    }
    
private:
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    std::unique_ptr<RemoteDramStorage> storage_;
    std::unique_ptr<AsyncOperationManager> async_manager_;
    std::unique_ptr<application::RemoteDramMessageHandler> app_handler_;
    std::unique_ptr<NetworkApplicationBridge> bridge_;
    ucp_ep_h server_endpoint_;
    ucp_ep_h client_endpoint_;
};

// Test clean interface between layers through bridge
TEST_F(Phase3IntegrationBridgeTests, CleanInterfaceValidation) {
    std::string test_key = "bridge_interface_test";
    std::string test_value = "bridge_test_data";
    
    // Create message through application layer
    auto message = CreateWriteRequestMessage(test_key, test_value, 90001);
    
    // Send through bridge interface (not direct network layer)
    auto status = bridge_->SendMessage(
        server_endpoint_,
        message.data(),
        message.size(),
        UCX_TAG_WRITE_REQUEST,
        CreateUserContext(90001)
    );
    
    EXPECT_TRUE(status.ok()) << "Bridge send failed: " << status;
    
    // Wait for complete processing through all layers
    WaitForCompleteBridgeProcessing(90001, 10000);
    
    // Verify data was stored through application layer
    auto stored_value = storage_->Get(test_key);
    EXPECT_TRUE(stored_value.has_value()) << "Data not stored through bridge";
    EXPECT_EQ(std::string(*stored_value), test_value);
    
    // Verify response was sent back through clean interface
    auto response = WaitForBridgeResponse(90001, 5000);
    EXPECT_TRUE(response.ok()) << "Bridge response failed: " << response.status();
}

// Test connection management through bridge
TEST_F(Phase3IntegrationBridgeTests, ConnectionManagement) {
    // Test connection creation through bridge
    ucp_ep_h new_endpoint;
    auto conn_status = bridge_->CreateConnection("127.0.0.1:12348", &new_endpoint);
    EXPECT_TRUE(conn_status.ok()) << "Bridge connection creation failed: " << conn_status;
    EXPECT_NE(new_endpoint, nullptr) << "Bridge returned null endpoint";
    
    // Test server startup through bridge
    auto server_status = bridge_->StartServer("127.0.0.1:12349");
    EXPECT_TRUE(server_status.ok()) << "Bridge server startup failed: " << server_status;
    
    // Verify connections work through bridge
    auto message = CreateWriteRequestMessage("conn_test", "data", 90002);
    auto send_status = bridge_->SendMessage(
        new_endpoint,
        message.data(),
        message.size(),
        UCX_TAG_WRITE_REQUEST,
        CreateUserContext(90002)
    );
    EXPECT_TRUE(send_status.ok()) << "Send through bridge connection failed: " << send_status;
}

// Test receive operations through bridge
TEST_F(Phase3IntegrationBridgeTests, ReceiveOperations) {
    // Post receive buffer through bridge
    auto receive_buffer = std::make_unique<char[]>(4096);
    auto receive_status = bridge_->PostReceive(
        receive_buffer.get(),
        4096,
        UCX_TAG_READ_RESPONSE,
        UCX_TAG_MASK,
        CreateReceiveContext(90003)
    );
    
    EXPECT_TRUE(receive_status.ok()) << "Bridge receive post failed: " << receive_status;
    
    // Send message that should be received
    auto test_message = CreateReadResponseMessage("test_data", 90003);
    SendDirectToEndpoint(client_endpoint_, test_message, UCX_TAG_READ_RESPONSE);
    
    // Wait for receive completion through bridge
    auto receive_result = WaitForReceiveCompletion(90003, 5000);
    EXPECT_TRUE(receive_result.ok()) << "Bridge receive completion failed: " << receive_result.status();
    
    // Verify data was received correctly
    EXPECT_EQ(receive_result.value().bytes_received, test_message.size());
}
```

#### 3.2 RemoteDramDriver Integration Tests
```cpp
// Test updated RemoteDramDriver with clean separation
TEST_F(Phase3IntegrationBridgeTests, RemoteDramDriverIntegration) {
    // Create driver using separated architecture
    auto driver_spec = RemoteDramDriverSpec::Make("127.0.0.1:12347");
    ASSERT_TRUE(driver_spec.ok());
    
    auto driver = RemoteDramDriver::CreateWithSeparatedLayers(
        driver_spec.value(), bridge_.get());
    ASSERT_TRUE(driver.ok()) << "Driver creation failed: " << driver.status();
    
    // Test TensorStore API compatibility
    kvstore::Key test_key = "driver_integration_test";
    absl::Cord test_value("driver_separated_test_data");
    
    // Test write operation
    auto write_future = driver.value()->Write(test_key, test_value, {});
    auto write_result = write_future.result();
    EXPECT_TRUE(write_result.ok()) << "Driver write failed: " << write_result.status();
    
    // Verify write timestamp
    EXPECT_NE(write_result.value().generation, StorageGeneration::Unknown());
    EXPECT_GT(write_result.value().time, absl::UnixEpoch());
    
    // Test read operation
    auto read_future = driver.value()->Read(test_key, {});
    auto read_result = read_future.result();
    EXPECT_TRUE(read_result.ok()) << "Driver read failed: " << read_result.status();
    
    // Verify read result
    EXPECT_EQ(read_result.value().state, kvstore::ReadResult::kValue);
    EXPECT_EQ(read_result.value().value, test_value);
    EXPECT_NE(read_result.value().stamp.generation, StorageGeneration::NoValue());
}

// Test concurrent operations through driver
TEST_F(Phase3IntegrationBridgeTests, ConcurrentDriverOperations) {
    auto driver_spec = RemoteDramDriverSpec::Make("127.0.0.1:12347");
    ASSERT_TRUE(driver_spec.ok());
    
    auto driver = RemoteDramDriver::CreateWithSeparatedLayers(
        driver_spec.value(), bridge_.get());
    ASSERT_TRUE(driver.ok());
    
    constexpr int kNumConcurrentOps = 25;
    std::vector<Future<TimestampedStorageGeneration>> write_futures;
    std::vector<Future<kvstore::ReadResult>> read_futures;
    
    // Launch concurrent write operations
    for (int i = 0; i < kNumConcurrentOps; ++i) {
        kvstore::Key key = absl::StrFormat("concurrent_driver_%d", i);
        absl::Cord value = absl::Cord(absl::StrFormat("concurrent_value_%d", i));
        
        auto write_future = driver.value()->Write(key, value, {});
        write_futures.push_back(std::move(write_future));
    }
    
    // Wait for all writes to complete
    for (auto& future : write_futures) {
        auto result = future.result();
        EXPECT_TRUE(result.ok()) << "Concurrent write failed: " << result.status();
    }
    
    // Launch concurrent read operations
    for (int i = 0; i < kNumConcurrentOps; ++i) {
        kvstore::Key key = absl::StrFormat("concurrent_driver_%d", i);
        auto read_future = driver.value()->Read(key, {});
        read_futures.push_back(std::move(read_future));
    }
    
    // Wait for all reads to complete
    for (int i = 0; i < kNumConcurrentOps; ++i) {
        auto result = read_futures[i].result();
        EXPECT_TRUE(result.ok()) << "Concurrent read " << i << " failed: " << result.status();
        
        std::string expected_value = absl::StrFormat("concurrent_value_%d", i);
        EXPECT_EQ(std::string(result.value().value), expected_value);
    }
}
```

#### 3.3 End-to-End Integration Tests
```cpp
// Test complete end-to-end flow through all separated layers
TEST_F(Phase3IntegrationBridgeTests, EndToEndSeparatedFlow) {
    // Set up monitoring for all layers
    NetworkLayerMonitor network_monitor;
    ApplicationLayerMonitor app_monitor;
    BridgeLayerMonitor bridge_monitor;
    
    network_layer_->SetMonitor(&network_monitor);
    app_handler_->SetMonitor(&app_monitor);
    bridge_->SetMonitor(&bridge_monitor);
    
    // Perform end-to-end operation
    std::string test_key = "e2e_separated_test";
    std::string test_value = "end_to_end_separated_data";
    
    auto message = CreateWriteRequestMessage(test_key, test_value, 90004);
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto status = bridge_->SendMessage(
        server_endpoint_,
        message.data(),
        message.size(),
        UCX_TAG_WRITE_REQUEST,
        CreateUserContext(90004)
    );
    
    EXPECT_TRUE(status.ok()) << "E2E send failed: " << status;
    
    // Wait for complete processing
    WaitForE2ECompletion(90004, 10000);
    auto end_time = std::chrono::high_resolution_clock::now();
    
    // Verify each layer performed its role correctly
    EXPECT_TRUE(network_monitor.HandledNetworkOperation()) << "Network layer didn't handle operation";
    EXPECT_LT(network_monitor.GetCallbackDuration(), std::chrono::microseconds(25))
        << "Network callback too slow";
    EXPECT_FALSE(network_monitor.PerformedApplicationWork()) << "Network layer did app work";
    
    EXPECT_TRUE(app_monitor.ProcessedMessage()) << "Application didn't process message";
    EXPECT_TRUE(app_monitor.StoredData()) << "Application didn't store data";
    EXPECT_TRUE(app_monitor.GeneratedResponse()) << "Application didn't generate response";
    
    EXPECT_TRUE(bridge_monitor.CoordinatedLayers()) << "Bridge didn't coordinate layers";
    EXPECT_EQ(bridge_monitor.GetEventTranslations(), 2) << "Bridge translation count wrong";
    
    // Verify final result
    auto stored_value = storage_->Get(test_key);
    EXPECT_TRUE(stored_value.has_value()) << "E2E data not stored";
    EXPECT_EQ(std::string(*stored_value), test_value);
    
    // Verify total latency is acceptable
    auto total_latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    EXPECT_LT(total_latency, std::chrono::milliseconds(15))
        << "E2E latency too high: " << total_latency.count() << "μs";
}

// Test performance with clean separation
TEST_F(Phase3IntegrationBridgeTests, SeparatedArchitecturePerformance) {
    constexpr int kNumOperations = 300;
    constexpr size_t kValueSize = 4096;  // 4KB values
    
    std::string test_value(kValueSize, 'S');  // 'S' for Separated
    std::vector<std::future<absl::Status>> operation_futures;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Launch operations through clean separated architecture
    for (int i = 0; i < kNumOperations; ++i) {
        auto future = std::async(std::launch::async, [this, i, test_value]() {
            std::string key = absl::StrFormat("separated_perf_%d", i);
            auto message = CreateWriteRequestMessage(key, test_value, 91000 + i);
            
            return bridge_->SendMessage(
                server_endpoint_,
                message.data(),
                message.size(),
                UCX_TAG_WRITE_REQUEST,
                CreateUserContext(91000 + i)
            );
        });
        
        operation_futures.push_back(std::move(future));
    }
    
    // Wait for all operations
    int successful_operations = 0;
    for (auto& future : operation_futures) {
        auto status = future.get();
        if (status.ok()) {
            successful_operations++;
        }
    }
    
    // Wait for all processing to complete
    for (int i = 0; i < successful_operations; ++i) {
        WaitForCompleteBridgeProcessing(91000 + i, 15000);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate throughput
    double throughput_mbps = (successful_operations * kValueSize) / (1024.0 * 1024.0) / 
                           (total_time.count() / 1000.0);
    
    // Expect at least 35 MB/s (accounting for clean separation overhead)
    EXPECT_GE(throughput_mbps, 35.0) 
        << "Separated architecture throughput: " << throughput_mbps << " MB/s";
    EXPECT_GE(successful_operations, kNumOperations * 0.95)  // 95% success rate
        << "Success rate: " << (100.0 * successful_operations / kNumOperations) << "%";
    
    // Verify storage consistency
    int stored_count = 0;
    for (int i = 0; i < kNumOperations; ++i) {
        std::string key = absl::StrFormat("separated_perf_%d", i);
        if (storage_->Exists(key)) {
            stored_count++;
        }
    }
    EXPECT_EQ(stored_count, successful_operations) << "Storage inconsistency detected";
    
    ABSL_LOG(INFO) << "Phase 3 separated architecture throughput: " << throughput_mbps << " MB/s"
                   << ", Success rate: " << (100.0 * successful_operations / kNumOperations) << "%"
                   << ", Stored: " << stored_count << "/" << kNumOperations;
}
```

#### Phase 3 Success Criteria
- [ ] **Clean Integration**: Bridge provides clean interface between network and application
- [ ] **Driver Compatibility**: RemoteDramDriver works with separated architecture
- [ ] **API Preservation**: All TensorStore KVStore APIs continue to work unchanged
- [ ] **Performance**: < 10% additional degradation compared to Phase 2 (≥35 MB/s)
- [ ] **Concurrent Operations**: 25+ concurrent operations work correctly
- [ ] **Layer Coordination**: Bridge correctly coordinates between all layers
- [ ] **End-to-End Latency**: Complete operations finish within 15ms
- [ ] **Storage Consistency**: 100% data consistency across all operations

---

## PHASE 4: Performance Optimization (Week 7-8)

### Goal
Optimize the separated architecture for maximum performance while maintaining clean separation.

### 4.1 Lock-Free Event Processing

```cpp
// NEW: High-performance event processing with clean separation
class HighPerformanceEventBridge {
private:
    // Lock-free event queue for network→application communication
    struct NetworkEvent {
        network::EventType type;
        ucp_ep_h endpoint;
        void* buffer;
        size_t bytes_transferred;
        uint64_t operation_id;
        void* user_context;
    };
    
    static constexpr size_t kEventQueueSize = 1024;
    std::array<std::atomic<NetworkEvent>, kEventQueueSize> event_queue_;
    std::atomic<size_t> producer_index_{0};
    std::atomic<size_t> consumer_index_{0};
    
public:
    // ✅ Fast event notification without locks
    void NotifyNetworkEvent(const network::NetworkEvent& event) {
        size_t index = producer_index_.fetch_add(1, std::memory_order_acq_rel) % kEventQueueSize;
        
        // Wait for slot to be available (lock-free)
        NetworkEvent* slot = &event_queue_[index];
        while (slot->type != network::EventType::NONE) {
            std::this_thread::yield();
        }
        
        // Store event (lock-free)
        slot->type = event.type;
        slot->endpoint = event.endpoint;
        slot->buffer = event.buffer;
        slot->bytes_transferred = event.bytes_transferred;
        slot->operation_id = event.operation_id;
        slot->user_context = event.user_context;
    }
    
    // ✅ Fast event processing by application
    void ProcessEvents(application::RemoteDramMessageHandler* handler) {
        size_t current_consumer = consumer_index_.load(std::memory_order_acquire);
        size_t current_producer = producer_index_.load(std::memory_order_acquire);
        
        while (current_consumer != current_producer) {
            size_t index = current_consumer % kEventQueueSize;
            NetworkEvent* slot = &event_queue_[index];
            
            if (slot->type != network::EventType::NONE) {
                // Convert to application event
                network::NetworkEvent app_event{
                    .type = slot->type,
                    .endpoint = slot->endpoint,
                    .buffer = slot->buffer,
                    .bytes_transferred = slot->bytes_transferred,
                    .operation_id = slot->operation_id,
                    .user_context = slot->user_context
                };
                
                // ✅ Application processes its own events
                handler->HandleNetworkEvent(app_event);
                
                // Mark slot as processed
                slot->type = network::EventType::NONE;
            }
            
            current_consumer++;
            consumer_index_.store(current_consumer, std::memory_order_release);
        }
    }
};
```

### 4.2 Dedicated Application Thread

```cpp
// NEW: Dedicated application processing thread for maximum performance
class DedicatedApplicationProcessor {
private:
    std::unique_ptr<std::thread> app_thread_;
    std::atomic<bool> should_stop_{false};
    HighPerformanceEventBridge* event_bridge_;
    application::RemoteDramMessageHandler* message_handler_;
    
public:
    void StartApplicationProcessor() {
        app_thread_ = std::make_unique<std::thread>(&DedicatedApplicationProcessor::ProcessorLoop, this);
        
        // Set CPU affinity for optimal performance
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(1, &cpuset);  // Use CPU core 1 for application processing
        pthread_setaffinity_np(app_thread_->native_handle(), sizeof(cpu_set_t), &cpuset);
    }
    
private:
    void ProcessorLoop() {
        while (!should_stop_.load(std::memory_order_acquire)) {
            // ✅ High-frequency application event processing
            event_bridge_->ProcessEvents(message_handler_);
            
            // ✅ Minimal sleep for optimal responsiveness
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
};
```

### Implementation Steps - Phase 4
1. **Week 7**: Implement high-performance event processing
2. **Week 7**: Add dedicated application processing thread
3. **Week 8**: Performance tuning and CPU affinity optimization
4. **Week 8**: Comprehensive performance benchmarking

### Phase 4 Testing and Validation

#### 4.1 Lock-Free Performance Tests
```cpp
// NEW: High-performance optimization tests for Phase 4
class Phase4PerformanceOptimizationTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize optimized separated architecture
        network_layer_ = std::make_unique<UcxNetworkLayer>();
        ASSERT_TRUE(network_layer_->Initialize().ok());
        
        storage_ = std::make_unique<RemoteDramStorage>();
        async_manager_ = std::make_unique<AsyncOperationManager>();
        
        app_handler_ = std::make_unique<application::RemoteDramMessageHandler>(
            storage_.get(), async_manager_.get());
        
        // Create high-performance event bridge
        hp_event_bridge_ = std::make_unique<HighPerformanceEventBridge>();
        network_layer_->SetEventBridge(hp_event_bridge_.get());
        
        // Create dedicated application processor
        app_processor_ = std::make_unique<DedicatedApplicationProcessor>(
            hp_event_bridge_.get(), app_handler_.get());
        
        bridge_ = std::make_unique<NetworkApplicationBridge>(
            network_layer_.get(), app_handler_.get());
        
        // Set up endpoints
        server_endpoint_ = SetupTestServer("127.0.0.1:12348");
        client_endpoint_ = SetupTestClient("127.0.0.1:12348");
        
        // Start optimized processing
        app_processor_->StartApplicationProcessor();
    }
    
    void TearDown() override {
        app_processor_->StopApplicationProcessor();
        bridge_.reset();
        network_layer_->Shutdown();
    }
    
private:
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    std::unique_ptr<RemoteDramStorage> storage_;
    std::unique_ptr<AsyncOperationManager> async_manager_;
    std::unique_ptr<application::RemoteDramMessageHandler> app_handler_;
    std::unique_ptr<HighPerformanceEventBridge> hp_event_bridge_;
    std::unique_ptr<DedicatedApplicationProcessor> app_processor_;
    std::unique_ptr<NetworkApplicationBridge> bridge_;
    ucp_ep_h server_endpoint_;
    ucp_ep_h client_endpoint_;
};

// Test lock-free event processing performance
TEST_F(Phase4PerformanceOptimizationTests, LockFreeEventProcessing) {
    constexpr int kNumEvents = 10000;
    constexpr int kConcurrentProducers = 4;
    
    std::atomic<int> events_processed{0};
    std::atomic<int> events_produced{0};
    
    // Start event consumers
    EventProcessingMonitor monitor;
    hp_event_bridge_->SetMonitor(&monitor);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Launch concurrent event producers
    std::vector<std::thread> producer_threads;
    for (int t = 0; t < kConcurrentProducers; ++t) {
        producer_threads.emplace_back([this, t, &events_produced, kNumEvents, kConcurrentProducers]() {
            int events_per_producer = kNumEvents / kConcurrentProducers;
            for (int i = 0; i < events_per_producer; ++i) {
                network::NetworkEvent event{};
                event.type = network::EventType::DATA_RECEIVED;
                event.operation_id = t * events_per_producer + i;
                event.bytes_transferred = 1024;
                
                hp_event_bridge_->NotifyNetworkEvent(event);
                events_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // Wait for all events to be produced
    for (auto& thread : producer_threads) {
        thread.join();
    }
    
    // Wait for all events to be processed
    while (monitor.GetProcessedEventCount() < kNumEvents) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // Calculate event processing rate
    double events_per_second = (kNumEvents * 1000000.0) / total_time.count();
    
    // Expect high-performance event processing (>100K events/sec)
    EXPECT_GT(events_per_second, 100000.0)
        << "Lock-free event processing rate: " << events_per_second << " events/sec";
    
    // Verify no events were lost
    EXPECT_EQ(monitor.GetProcessedEventCount(), kNumEvents)
        << "Event loss detected: " << monitor.GetProcessedEventCount() << "/" << kNumEvents;
    
    // Verify lock-free operation (no lock contention)
    EXPECT_EQ(monitor.GetLockContentionCount(), 0)
        << "Lock contention detected in lock-free processing";
    
    ABSL_LOG(INFO) << "Phase 4 lock-free event processing: " << events_per_second << " events/sec";
}

// Test dedicated application thread performance
TEST_F(Phase4PerformanceOptimizationTests, DedicatedApplicationThreadPerformance) {
    constexpr int kNumOperations = 1000;
    constexpr size_t kValueSize = 8192;  // 8KB values
    
    std::string test_value(kValueSize, 'P');  // 'P' for Performance
    
    ThreadAffinityMonitor affinity_monitor;
    app_processor_->SetAffinityMonitor(&affinity_monitor);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Launch high-throughput operations
    std::vector<std::future<absl::Status>> operation_futures;
    for (int i = 0; i < kNumOperations; ++i) {
        auto future = std::async(std::launch::async, [this, i, test_value]() {
            std::string key = absl::StrFormat("hp_perf_%d", i);
            auto message = CreateWriteRequestMessage(key, test_value, 100000 + i);
            
            return bridge_->SendMessage(
                server_endpoint_,
                message.data(),
                message.size(),
                UCX_TAG_WRITE_REQUEST,
                CreateUserContext(100000 + i)
            );
        });
        
        operation_futures.push_back(std::move(future));
    }
    
    // Wait for all operations
    int successful_operations = 0;
    for (auto& future : operation_futures) {
        auto status = future.get();
        if (status.ok()) {
            successful_operations++;
        }
    }
    
    // Wait for all processing to complete
    for (int i = 0; i < successful_operations; ++i) {
        WaitForOptimizedProcessing(100000 + i, 10000);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate throughput
    double throughput_mbps = (successful_operations * kValueSize) / (1024.0 * 1024.0) / 
                           (total_time.count() / 1000.0);
    
    // Expect significant performance improvement (≥80 MB/s)
    EXPECT_GE(throughput_mbps, 80.0) 
        << "Optimized architecture throughput: " << throughput_mbps << " MB/s";
    EXPECT_GE(successful_operations, kNumOperations * 0.98)  // 98% success rate
        << "Success rate: " << (100.0 * successful_operations / kNumOperations) << "%";
    
    // Verify CPU affinity is working
    EXPECT_TRUE(affinity_monitor.IsAffinityMaintained())
        << "CPU affinity not maintained for application thread";
    EXPECT_GT(affinity_monitor.GetCacheHitRate(), 0.95)  // >95% cache hit rate
        << "Cache hit rate: " << affinity_monitor.GetCacheHitRate();
    
    // Verify storage consistency
    int stored_count = 0;
    for (int i = 0; i < kNumOperations; ++i) {
        std::string key = absl::StrFormat("hp_perf_%d", i);
        if (storage_->Exists(key)) {
            stored_count++;
        }
    }
    EXPECT_EQ(stored_count, successful_operations) << "Storage inconsistency";
    
    ABSL_LOG(INFO) << "Phase 4 optimized throughput: " << throughput_mbps << " MB/s"
                   << ", Success rate: " << (100.0 * successful_operations / kNumOperations) << "%"
                   << ", Cache hit rate: " << affinity_monitor.GetCacheHitRate();
}

// Test CPU usage optimization
TEST_F(Phase4PerformanceOptimizationTests, CPUUsageOptimization) {
    constexpr int kTestDurationSeconds = 10;
    constexpr int kOperationsPerSecond = 500;
    
    CPUUsageMonitor cpu_monitor;
    cpu_monitor.StartMonitoring();
    
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(kTestDurationSeconds);
    
    int operation_count = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        std::string key = absl::StrFormat("cpu_test_%d", operation_count);
        std::string value = "cpu_optimization_test_data";
        auto message = CreateWriteRequestMessage(key, value, 110000 + operation_count);
        
        auto status = bridge_->SendMessage(
            server_endpoint_,
            message.data(),
            message.size(),
            UCX_TAG_WRITE_REQUEST,
            CreateUserContext(110000 + operation_count)
        );
        
        if (status.ok()) {
            operation_count++;
        }
        
        // Maintain target rate
        std::this_thread::sleep_for(std::chrono::microseconds(1000000 / kOperationsPerSecond));
    }
    
    cpu_monitor.StopMonitoring();
    
    // Verify CPU usage is optimized
    double network_thread_cpu = cpu_monitor.GetNetworkThreadCPU();
    double app_thread_cpu = cpu_monitor.GetApplicationThreadCPU();
    double total_cpu = cpu_monitor.GetTotalCPU();
    
    // Network thread should use <5% CPU (optimized progress polling)
    EXPECT_LT(network_thread_cpu, 5.0)
        << "Network thread CPU usage: " << network_thread_cpu << "%";
    
    // Application thread should be efficient but can use more CPU
    EXPECT_LT(app_thread_cpu, 25.0)
        << "Application thread CPU usage: " << app_thread_cpu << "%";
    
    // Total CPU usage should be reasonable
    EXPECT_LT(total_cpu, 30.0)
        << "Total CPU usage: " << total_cpu << "%";
    
    // Verify operations completed successfully
    EXPECT_GT(operation_count, kTestDurationSeconds * kOperationsPerSecond * 0.9)
        << "Operation rate too low: " << (operation_count / kTestDurationSeconds) << " ops/sec";
    
    ABSL_LOG(INFO) << "Phase 4 CPU usage - Network: " << network_thread_cpu << "%"
                   << ", Application: " << app_thread_cpu << "%"
                   << ", Total: " << total_cpu << "%"
                   << ", Operations: " << operation_count;
}
```

#### 4.2 Memory and Resource Optimization Tests
```cpp
// Test memory usage optimization
TEST_F(Phase4PerformanceOptimizationTests, MemoryOptimization) {
    MemoryUsageMonitor memory_monitor;
    memory_monitor.StartMonitoring();
    
    size_t initial_memory = memory_monitor.GetCurrentUsage();
    
    constexpr int kNumOperations = 2000;
    constexpr size_t kValueSize = 4096;  // 4KB values
    
    std::string test_value(kValueSize, 'M');  // 'M' for Memory
    
    // Perform memory-intensive operations
    for (int batch = 0; batch < 10; ++batch) {
        std::vector<std::future<absl::Status>> batch_futures;
        
        // Launch batch of operations
        for (int i = 0; i < kNumOperations / 10; ++i) {
            int op_id = batch * (kNumOperations / 10) + i;
            auto future = std::async(std::launch::async, [this, op_id, test_value]() {
                std::string key = absl::StrFormat("mem_opt_%d", op_id);
                auto message = CreateWriteRequestMessage(key, test_value, 120000 + op_id);
                
                return bridge_->SendMessage(
                    server_endpoint_,
                    message.data(),
                    message.size(),
                    UCX_TAG_WRITE_REQUEST,
                    CreateUserContext(120000 + op_id)
                );
            });
            
            batch_futures.push_back(std::move(future));
        }
        
        // Wait for batch completion
        for (auto& future : batch_futures) {
            future.get();
        }
        
        // Force cleanup between batches
        network_layer_->CleanupResources();
        app_handler_->CleanupResources();
        hp_event_bridge_->CleanupResources();
        
        size_t batch_memory = memory_monitor.GetCurrentUsage();
        ABSL_LOG(INFO) << "Batch " << batch << " memory usage: " << batch_memory << " bytes";
    }
    
    size_t final_memory = memory_monitor.GetCurrentUsage();
    size_t memory_increase = final_memory - initial_memory;
    
    // Expected increase: stored data + reasonable overhead
    size_t expected_data_size = kNumOperations * kValueSize;
    size_t max_acceptable_overhead = expected_data_size;  // 100% overhead max
    
    EXPECT_LT(memory_increase, expected_data_size + max_acceptable_overhead)
        << "Memory usage too high: " << memory_increase << " bytes"
        << " (expected < " << (expected_data_size + max_acceptable_overhead) << ")";
    
    // Verify memory efficiency
    double memory_efficiency = static_cast<double>(expected_data_size) / memory_increase;
    EXPECT_GT(memory_efficiency, 0.5)  // At least 50% efficiency
        << "Memory efficiency: " << memory_efficiency;
    
    memory_monitor.StopMonitoring();
    
    ABSL_LOG(INFO) << "Phase 4 memory optimization - Initial: " << initial_memory
                   << ", Final: " << final_memory
                   << ", Increase: " << memory_increase
                   << ", Efficiency: " << memory_efficiency;
}

// Test resource cleanup and leak detection
TEST_F(Phase4PerformanceOptimizationTests, ResourceLeakDetection) {
    ResourceLeakDetector leak_detector;
    leak_detector.StartDetection();
    
    constexpr int kNumCycles = 50;
    constexpr int kOperationsPerCycle = 100;
    
    for (int cycle = 0; cycle < kNumCycles; ++cycle) {
        std::vector<std::future<absl::Status>> cycle_futures;
        
        // Create and destroy resources in each cycle
        for (int i = 0; i < kOperationsPerCycle; ++i) {
            int op_id = cycle * kOperationsPerCycle + i;
            auto future = std::async(std::launch::async, [this, op_id]() {
                std::string key = absl::StrFormat("leak_test_%d", op_id);
                std::string value = "resource_leak_test_data";
                auto message = CreateWriteRequestMessage(key, value, 130000 + op_id);
                
                return bridge_->SendMessage(
                    server_endpoint_,
                    message.data(),
                    message.size(),
                    UCX_TAG_WRITE_REQUEST,
                    CreateUserContext(130000 + op_id)
                );
            });
            
            cycle_futures.push_back(std::move(future));
        }
        
        // Wait for cycle completion
        for (auto& future : cycle_futures) {
            future.get();
        }
        
        // Force aggressive cleanup after each cycle
        network_layer_->ForceCleanup();
        app_handler_->ForceCleanup();
        hp_event_bridge_->ForceCleanup();
        
        // Check for leaks
        auto leak_report = leak_detector.GenerateReport();
        EXPECT_EQ(leak_report.leaked_network_resources, 0)
            << "Network resource leak in cycle " << cycle;
        EXPECT_EQ(leak_report.leaked_application_resources, 0)
            << "Application resource leak in cycle " << cycle;
        EXPECT_EQ(leak_report.leaked_bridge_resources, 0)
            << "Bridge resource leak in cycle " << cycle;
    }
    
    leak_detector.StopDetection();
    auto final_report = leak_detector.GenerateFinalReport();
    
    // Verify no resource leaks
    EXPECT_EQ(final_report.total_leaked_resources, 0)
        << "Resource leaks detected: " << final_report.total_leaked_resources;
    EXPECT_GT(final_report.cleanup_efficiency, 0.99)  // 99% cleanup efficiency
        << "Cleanup efficiency: " << final_report.cleanup_efficiency;
    
    ABSL_LOG(INFO) << "Phase 4 resource leak detection completed - "
                   << "Cycles: " << kNumCycles
                   << ", Operations: " << (kNumCycles * kOperationsPerCycle)
                   << ", Leaked resources: " << final_report.total_leaked_resources
                   << ", Cleanup efficiency: " << final_report.cleanup_efficiency;
}
```

#### Phase 4 Success Criteria
- [ ] **Lock-Free Performance**: >100K events/sec processing rate with zero lock contention
- [ ] **Throughput Improvement**: ≥80 MB/s sustained throughput (2.3x improvement over Phase 3)
- [ ] **CPU Optimization**: <5% network thread CPU, <30% total CPU usage
- [ ] **Memory Efficiency**: >50% memory efficiency, zero resource leaks
- [ ] **Thread Affinity**: >95% cache hit rate with dedicated CPU cores
- [ ] **Concurrent Operations**: 1K+ concurrent operations without degradation
- [ ] **Response Latency**: <5ms average response time for 8KB operations
- [ ] **Resource Management**: 99%+ cleanup efficiency, zero leaked resources

---

## PHASE 5: Migration and Testing (Week 9-10)

### Goal
Safely migrate existing implementation to new separated architecture with comprehensive testing.

### 5.1 Feature Flag Migration

```cpp
// NEW: Feature flag system for safe migration
namespace feature_flags {

enum class ArchitectureMode {
    LEGACY_MIXED,      // Current implementation (UCX + application mixed)
    SEPARATED_LAYERS   // New separated implementation
};

class ArchitectureMigration {
public:
    static ArchitectureMode GetCurrentMode() {
        static ArchitectureMode mode = ReadModeFromConfig();
        return mode;
    }
    
    static void SetMode(ArchitectureMode new_mode) {
        ABSL_LOG(INFO) << "Switching architecture mode from " 
                      << ModeToString(GetCurrentMode()) 
                      << " to " << ModeToString(new_mode);
        // Implementation details for safe mode switching
    }
    
private:
    static ArchitectureMode ReadModeFromConfig() {
        // Read from environment variable or config file
        const char* mode_str = getenv("REMOTE_DRAM_ARCHITECTURE_MODE");
        if (mode_str && strcmp(mode_str, "separated") == 0) {
            return ArchitectureMode::SEPARATED_LAYERS;
        }
        return ArchitectureMode::LEGACY_MIXED;  // Safe default
    }
};

}  // namespace feature_flags
```

### 5.2 Comprehensive Testing Suite

```cpp
// NEW: Comprehensive testing for architectural separation
class ArchitectureSeparationTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up both legacy and separated architectures for comparison
        legacy_impl_ = CreateLegacyImplementation();
        separated_impl_ = CreateSeparatedImplementation();
    }
    
public:
    // Test that network callbacks are fast in separated architecture
    TEST_F(ArchitectureSeparationTests, NetworkCallbackLatency) {
        auto legacy_latency = MeasureCallbackLatency(legacy_impl_.get());
        auto separated_latency = MeasureCallbackLatency(separated_impl_.get());
        
        EXPECT_LT(separated_latency, legacy_latency / 10);  // 10x improvement
        EXPECT_LT(separated_latency, std::chrono::microseconds(25));  // <25μs absolute
    }
    
    // Test that application logic doesn't block network progress
    TEST_F(ArchitectureSeparationTests, NetworkProgressIsolation) {
        // Inject slow application logic
        separated_impl_->InjectSlowApplicationLogic(std::chrono::milliseconds(100));
        
        // Network operations should still complete quickly
        auto network_latency = MeasureNetworkOperationLatency(separated_impl_.get());
        EXPECT_LT(network_latency, std::chrono::microseconds(50));
    }
    
    // Test error isolation between layers
    TEST_F(ArchitectureSeparationTests, ErrorIsolation) {
        // Application error should not crash network layer
        separated_impl_->InjectApplicationError();
        
        EXPECT_TRUE(separated_impl_->IsNetworkLayerHealthy());
        EXPECT_TRUE(separated_impl_->CanProcessNetworkOperations());
    }
    
    // Performance comparison test
    TEST_F(ArchitectureSeparationTests, ThroughputComparison) {
        auto legacy_throughput = MeasureThroughput(legacy_impl_.get());
        auto separated_throughput = MeasureThroughput(separated_impl_.get());
        
        EXPECT_GE(separated_throughput, legacy_throughput * 1.2);  // 20% improvement minimum
    }
};
```

### Implementation Steps - Phase 5
1. **Week 9**: Implement feature flag system and migration path
2. **Week 9**: Create comprehensive test suite for both architectures
3. **Week 10**: Performance regression testing and optimization
4. **Week 10**: Documentation and deployment preparation

### Phase 5 Testing and Validation

#### 5.1 Feature Flag and Migration Tests
```cpp
// NEW: Comprehensive migration and feature flag tests for Phase 5
class Phase5MigrationAndTestingTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize both legacy and separated architectures for comparison
        legacy_impl_ = CreateLegacyImplementation();
        separated_impl_ = CreateSeparatedImplementation();
        
        migration_manager_ = std::make_unique<feature_flags::ArchitectureMigration>();
        
        // Set up endpoints
        server_endpoint_ = SetupTestServer("127.0.0.1:12349");
        client_endpoint_ = SetupTestClient("127.0.0.1:12349");
    }
    
    void TearDown() override {
        legacy_impl_.reset();
        separated_impl_.reset();
        migration_manager_.reset();
    }
    
private:
    std::unique_ptr<LegacyRemoteDramImplementation> legacy_impl_;
    std::unique_ptr<SeparatedRemoteDramImplementation> separated_impl_;
    std::unique_ptr<feature_flags::ArchitectureMigration> migration_manager_;
    ucp_ep_h server_endpoint_;
    ucp_ep_h client_endpoint_;
};

// Test feature flag system functionality
TEST_F(Phase5MigrationAndTestingTests, FeatureFlagSystem) {
    // Test default mode (should be legacy for safety)
    auto initial_mode = migration_manager_->GetCurrentMode();
    EXPECT_EQ(initial_mode, feature_flags::ArchitectureMode::LEGACY_MIXED)
        << "Default mode should be LEGACY_MIXED for safety";
    
    // Test mode switching
    migration_manager_->SetMode(feature_flags::ArchitectureMode::SEPARATED_LAYERS);
    auto switched_mode = migration_manager_->GetCurrentMode();
    EXPECT_EQ(switched_mode, feature_flags::ArchitectureMode::SEPARATED_LAYERS)
        << "Mode switching failed";
    
    // Test environment variable override
    setenv("REMOTE_DRAM_ARCHITECTURE_MODE", "separated", 1);
    migration_manager_->ReloadConfiguration();
    auto env_mode = migration_manager_->GetCurrentMode();
    EXPECT_EQ(env_mode, feature_flags::ArchitectureMode::SEPARATED_LAYERS)
        << "Environment variable override failed";
    
    // Reset environment
    unsetenv("REMOTE_DRAM_ARCHITECTURE_MODE");
    
    // Test rollback capability
    migration_manager_->SetMode(feature_flags::ArchitectureMode::LEGACY_MIXED);
    auto rollback_mode = migration_manager_->GetCurrentMode();
    EXPECT_EQ(rollback_mode, feature_flags::ArchitectureMode::LEGACY_MIXED)
        << "Rollback to legacy mode failed";
}

// Test migration between architectures
TEST_F(Phase5MigrationAndTestingTests, ArchitectureMigration) {
    // Start with legacy architecture
    migration_manager_->SetMode(feature_flags::ArchitectureMode::LEGACY_MIXED);
    auto legacy_driver = CreateDriverWithCurrentMode();
    ASSERT_TRUE(legacy_driver.ok()) << "Legacy driver creation failed";
    
    // Perform operations in legacy mode
    kvstore::Key test_key = "migration_test_key";
    absl::Cord test_value("migration_test_data");
    
    auto legacy_write = legacy_driver.value()->Write(test_key, test_value, {});
    auto legacy_write_result = legacy_write.result();
    EXPECT_TRUE(legacy_write_result.ok()) << "Legacy write failed: " << legacy_write_result.status();
    
    // Migrate to separated architecture
    migration_manager_->SetMode(feature_flags::ArchitectureMode::SEPARATED_LAYERS);
    auto separated_driver = CreateDriverWithCurrentMode();
    ASSERT_TRUE(separated_driver.ok()) << "Separated driver creation failed";
    
    // Verify data persistence across migration
    auto separated_read = separated_driver.value()->Read(test_key, {});
    auto separated_read_result = separated_read.result();
    EXPECT_TRUE(separated_read_result.ok()) << "Read after migration failed: " << separated_read_result.status();
    EXPECT_EQ(separated_read_result.value().value, test_value) << "Data not preserved during migration";
    
    // Perform new operations in separated mode
    kvstore::Key new_key = "post_migration_key";
    absl::Cord new_value("post_migration_data");
    
    auto separated_write = separated_driver.value()->Write(new_key, new_value, {});
    auto separated_write_result = separated_write.result();
    EXPECT_TRUE(separated_write_result.ok()) << "Separated write failed: " << separated_write_result.status();
    
    // Test rollback migration
    migration_manager_->SetMode(feature_flags::ArchitectureMode::LEGACY_MIXED);
    auto rollback_driver = CreateDriverWithCurrentMode();
    ASSERT_TRUE(rollback_driver.ok()) << "Rollback driver creation failed";
    
    // Verify both data entries are still accessible
    auto rollback_read1 = rollback_driver.value()->Read(test_key, {});
    auto rollback_read2 = rollback_driver.value()->Read(new_key, {});
    
    EXPECT_TRUE(rollback_read1.result().ok()) << "Original data not accessible after rollback";
    EXPECT_TRUE(rollback_read2.result().ok()) << "New data not accessible after rollback";
    EXPECT_EQ(rollback_read1.result().value().value, test_value);
    EXPECT_EQ(rollback_read2.result().value().value, new_value);
}

// Test automatic rollback on performance issues
TEST_F(Phase5MigrationAndTestingTests, AutomaticRollback) {
    // Enable automatic performance monitoring
    PerformanceRegressionDetector detector;
    detector.SetThroughputThreshold(30.0);  // MB/s minimum
    detector.SetLatencyThreshold(std::chrono::milliseconds(20));  // 20ms max
    detector.SetFailureRateThreshold(0.05);  // 5% max
    
    migration_manager_->SetPerformanceDetector(&detector);
    migration_manager_->SetMode(feature_flags::ArchitectureMode::SEPARATED_LAYERS);
    
    // Inject performance degradation
    PerformanceDegradationInjector injector;
    injector.InjectThroughputDegradation(0.5);  // 50% reduction
    injector.InjectLatencyIncrease(2.0);        // 2x increase
    
    auto degraded_driver = CreateDriverWithCurrentMode();
    ASSERT_TRUE(degraded_driver.ok());
    
    // Perform operations that should trigger rollback
    constexpr int kNumOperations = 100;
    std::vector<Future<TimestampedStorageGeneration>> write_futures;
    
    for (int i = 0; i < kNumOperations; ++i) {
        kvstore::Key key = absl::StrFormat("rollback_test_%d", i);
        absl::Cord value = absl::Cord("rollback_test_data");
        
        auto write_future = degraded_driver.value()->Write(key, value, {});
        write_futures.push_back(std::move(write_future));
    }
    
    // Wait for operations and performance detection
    for (auto& future : write_futures) {
        future.result();  // Don't check success, expect some failures
    }
    
    detector.ProcessPerformanceData();
    
    // Wait for automatic rollback
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Verify automatic rollback occurred
    auto current_mode = migration_manager_->GetCurrentMode();
    EXPECT_EQ(current_mode, feature_flags::ArchitectureMode::LEGACY_MIXED)
        << "Automatic rollback should have occurred due to performance degradation";
    
    // Verify system is functional after rollback
    auto post_rollback_driver = CreateDriverWithCurrentMode();
    ASSERT_TRUE(post_rollback_driver.ok());
    
    kvstore::Key test_key = "post_rollback_test";
    absl::Cord test_value = absl::Cord("post_rollback_data");
    
    auto write_result = post_rollback_driver.value()->Write(test_key, test_value, {}).result();
    EXPECT_TRUE(write_result.ok()) << "System not functional after rollback";
    
    auto read_result = post_rollback_driver.value()->Read(test_key, {}).result();
    EXPECT_TRUE(read_result.ok()) << "Read not functional after rollback";
    EXPECT_EQ(read_result.value().value, test_value);
}
```

#### 5.2 Comprehensive Comparison Tests
```cpp
// Test comprehensive comparison between legacy and separated architectures
TEST_F(Phase5MigrationAndTestingTests, ComprehensiveArchitectureComparison) {
    ComparisonTestSuite test_suite;
    
    // Functional correctness comparison
    auto functional_results = test_suite.RunFunctionalTests(legacy_impl_.get(), separated_impl_.get());
    
    EXPECT_EQ(functional_results.legacy_success_rate, functional_results.separated_success_rate)
        << "Functional correctness mismatch - Legacy: " << functional_results.legacy_success_rate
        << ", Separated: " << functional_results.separated_success_rate;
    
    EXPECT_GT(functional_results.legacy_success_rate, 0.99)  // 99% success rate minimum
        << "Legacy architecture success rate too low: " << functional_results.legacy_success_rate;
    EXPECT_GT(functional_results.separated_success_rate, 0.99)  // 99% success rate minimum
        << "Separated architecture success rate too low: " << functional_results.separated_success_rate;
    
    // Performance comparison
    auto perf_results = test_suite.RunPerformanceTests(legacy_impl_.get(), separated_impl_.get());
    
    // Separated should be significantly faster
    EXPECT_GT(perf_results.separated_throughput, perf_results.legacy_throughput * 1.5)  // 50% improvement minimum
        << "Separated architecture throughput: " << perf_results.separated_throughput << " MB/s"
        << ", Legacy: " << perf_results.legacy_throughput << " MB/s";
    
    // Latency should be lower in separated architecture
    EXPECT_LT(perf_results.separated_avg_latency, perf_results.legacy_avg_latency * 0.7)  // 30% reduction minimum
        << "Separated latency: " << perf_results.separated_avg_latency.count() << "μs"
        << ", Legacy: " << perf_results.legacy_avg_latency.count() << "μs";
    
    // Resource usage comparison
    auto resource_results = test_suite.RunResourceTests(legacy_impl_.get(), separated_impl_.get());
    
    // CPU usage should be lower in separated architecture
    EXPECT_LT(resource_results.separated_cpu_usage, resource_results.legacy_cpu_usage * 0.6)  // 40% reduction
        << "Separated CPU: " << resource_results.separated_cpu_usage << "%"
        << ", Legacy: " << resource_results.legacy_cpu_usage << "%";
    
    // Memory efficiency should be better
    EXPECT_GT(resource_results.separated_memory_efficiency, resource_results.legacy_memory_efficiency * 1.2)  // 20% improvement
        << "Separated memory efficiency: " << resource_results.separated_memory_efficiency
        << ", Legacy: " << resource_results.legacy_memory_efficiency;
    
    ABSL_LOG(INFO) << "Comprehensive comparison results:"
                   << "\n  Throughput - Legacy: " << perf_results.legacy_throughput << " MB/s"
                   << ", Separated: " << perf_results.separated_throughput << " MB/s"
                   << "\n  Latency - Legacy: " << perf_results.legacy_avg_latency.count() << "μs"
                   << ", Separated: " << perf_results.separated_avg_latency.count() << "μs"
                   << "\n  CPU Usage - Legacy: " << resource_results.legacy_cpu_usage << "%"
                   << ", Separated: " << resource_results.separated_cpu_usage << "%";
}

// Test backward compatibility preservation
TEST_F(Phase5MigrationAndTestingTests, BackwardCompatibilityPreservation) {
    // Test that all existing TensorStore APIs work identically in both architectures
    TensorStoreAPICompatibilityTester api_tester;
    
    // Test basic KVStore operations
    auto kvstore_results = api_tester.TestKVStoreAPIs(legacy_impl_.get(), separated_impl_.get());
    EXPECT_TRUE(kvstore_results.apis_identical) << "KVStore API behavior differs between architectures";
    EXPECT_EQ(kvstore_results.legacy_failures, 0) << "Legacy KVStore API failures: " << kvstore_results.legacy_failures;
    EXPECT_EQ(kvstore_results.separated_failures, 0) << "Separated KVStore API failures: " << kvstore_results.separated_failures;
    
    // Test driver specification parsing
    auto spec_results = api_tester.TestDriverSpecAPIs(legacy_impl_.get(), separated_impl_.get());
    EXPECT_TRUE(spec_results.specs_identical) << "Driver spec behavior differs between architectures";
    
    // Test error handling consistency
    auto error_results = api_tester.TestErrorHandling(legacy_impl_.get(), separated_impl_.get());
    EXPECT_TRUE(error_results.error_handling_identical) << "Error handling differs between architectures";
    
    // Test concurrent operation behavior
    auto concurrency_results = api_tester.TestConcurrencyAPIs(legacy_impl_.get(), separated_impl_.get());
    EXPECT_TRUE(concurrency_results.concurrency_identical) << "Concurrency behavior differs between architectures";
    
    ABSL_LOG(INFO) << "Backward compatibility test results:"
                   << "\n  KVStore APIs identical: " << kvstore_results.apis_identical
                   << "\n  Driver specs identical: " << spec_results.specs_identical
                   << "\n  Error handling identical: " << error_results.error_handling_identical
                   << "\n  Concurrency identical: " << concurrency_results.concurrency_identical;
}
```

#### 5.3 Production Readiness Tests
```cpp
// Test production readiness and deployment preparation
TEST_F(Phase5MigrationAndTestingTests, ProductionReadinessValidation) {
    ProductionReadinessValidator validator;
    
    // Test system stability under load
    auto stability_results = validator.TestStabilityUnderLoad(separated_impl_.get());
    EXPECT_GT(stability_results.uptime_percentage, 99.9)  // 99.9% uptime
        << "System stability too low: " << stability_results.uptime_percentage << "%";
    EXPECT_LT(stability_results.crash_rate, 0.001)  // <0.1% crash rate
        << "Crash rate too high: " << stability_results.crash_rate;
    EXPECT_EQ(stability_results.memory_leaks, 0) << "Memory leaks detected: " << stability_results.memory_leaks;
    
    // Test graceful degradation
    auto degradation_results = validator.TestGracefulDegradation(separated_impl_.get());
    EXPECT_TRUE(degradation_results.handles_network_failures) << "Doesn't handle network failures gracefully";
    EXPECT_TRUE(degradation_results.handles_memory_pressure) << "Doesn't handle memory pressure gracefully";
    EXPECT_TRUE(degradation_results.handles_high_load) << "Doesn't handle high load gracefully";
    
    // Test monitoring and observability
    auto monitoring_results = validator.TestMonitoringCapabilities(separated_impl_.get());
    EXPECT_TRUE(monitoring_results.has_performance_metrics) << "Missing performance metrics";
    EXPECT_TRUE(monitoring_results.has_error_tracking) << "Missing error tracking";
    EXPECT_TRUE(monitoring_results.has_resource_monitoring) << "Missing resource monitoring";
    EXPECT_GT(monitoring_results.metric_accuracy, 0.95)  // 95% accuracy
        << "Metric accuracy too low: " << monitoring_results.metric_accuracy;
    
    // Test deployment compatibility
    auto deployment_results = validator.TestDeploymentCompatibility(separated_impl_.get());
    EXPECT_TRUE(deployment_results.container_compatible) << "Not container compatible";
    EXPECT_TRUE(deployment_results.cloud_ready) << "Not cloud ready";
    EXPECT_TRUE(deployment_results.scaling_capable) << "Not scaling capable";
    
    // Test security and compliance
    auto security_results = validator.TestSecurityCompliance(separated_impl_.get());
    EXPECT_TRUE(security_results.no_security_vulnerabilities) << "Security vulnerabilities detected";
    EXPECT_TRUE(security_results.proper_error_handling) << "Improper error handling detected";
    EXPECT_TRUE(security_results.no_information_leakage) << "Information leakage detected";
    
    ABSL_LOG(INFO) << "Production readiness validation results:"
                   << "\n  Uptime: " << stability_results.uptime_percentage << "%"
                   << "\n  Crash rate: " << stability_results.crash_rate
                   << "\n  Memory leaks: " << stability_results.memory_leaks
                   << "\n  Metric accuracy: " << monitoring_results.metric_accuracy
                   << "\n  Container compatible: " << deployment_results.container_compatible
                   << "\n  Security compliant: " << security_results.no_security_vulnerabilities;
}

// Test final performance targets achievement
TEST_F(Phase5MigrationAndTestingTests, FinalPerformanceTargetsValidation) {
    FinalPerformanceValidator perf_validator;
    
    // Test that all phase targets are met
    auto phase1_validation = perf_validator.ValidatePhase1Targets(separated_impl_.get());
    EXPECT_LT(phase1_validation.callback_latency, std::chrono::microseconds(25))
        << "Phase 1 callback latency target not met: " << phase1_validation.callback_latency.count() << "μs";
    EXPECT_TRUE(phase1_validation.callback_purity) << "Phase 1 callback purity not achieved";
    
    auto phase2_validation = perf_validator.ValidatePhase2Targets(separated_impl_.get());
    EXPECT_TRUE(phase2_validation.complete_separation) << "Phase 2 complete separation not achieved";
    EXPECT_GE(phase2_validation.throughput_mbps, 40.0)
        << "Phase 2 throughput target not met: " << phase2_validation.throughput_mbps << " MB/s";
    
    auto phase3_validation = perf_validator.ValidatePhase3Targets(separated_impl_.get());
    EXPECT_TRUE(phase3_validation.clean_integration) << "Phase 3 clean integration not achieved";
    EXPECT_GE(phase3_validation.throughput_mbps, 35.0)
        << "Phase 3 throughput target not met: " << phase3_validation.throughput_mbps << " MB/s";
    
    auto phase4_validation = perf_validator.ValidatePhase4Targets(separated_impl_.get());
    EXPECT_GE(phase4_validation.event_processing_rate, 100000.0)
        << "Phase 4 event processing target not met: " << phase4_validation.event_processing_rate << " events/s";
    EXPECT_GE(phase4_validation.throughput_mbps, 80.0)
        << "Phase 4 throughput target not met: " << phase4_validation.throughput_mbps << " MB/s";
    EXPECT_LT(phase4_validation.cpu_usage_percent, 30.0)
        << "Phase 4 CPU usage target not met: " << phase4_validation.cpu_usage_percent << "%";
    
    // Test overall improvement targets
    auto overall_validation = perf_validator.ValidateOverallTargets(legacy_impl_.get(), separated_impl_.get());
    EXPECT_GE(overall_validation.throughput_improvement, 2.0)  // 2x improvement minimum
        << "Overall throughput improvement target not met: " << overall_validation.throughput_improvement << "x";
    EXPECT_GE(overall_validation.latency_improvement, 5.0)  // 5x improvement minimum
        << "Overall latency improvement target not met: " << overall_validation.latency_improvement << "x";
    EXPECT_GE(overall_validation.cpu_improvement, 2.5)  // 2.5x improvement minimum
        << "Overall CPU improvement target not met: " << overall_validation.cpu_improvement << "x";
    
    ABSL_LOG(INFO) << "Final performance targets validation:"
                   << "\n  Callback latency: " << phase1_validation.callback_latency.count() << "μs (target: <25μs)"
                   << "\n  Phase 2 throughput: " << phase2_validation.throughput_mbps << " MB/s (target: ≥40)"
                   << "\n  Phase 3 throughput: " << phase3_validation.throughput_mbps << " MB/s (target: ≥35)"
                   << "\n  Phase 4 throughput: " << phase4_validation.throughput_mbps << " MB/s (target: ≥80)"
                   << "\n  Event processing: " << phase4_validation.event_processing_rate << " events/s (target: ≥100K)"
                   << "\n  Overall improvements - Throughput: " << overall_validation.throughput_improvement << "x"
                   << ", Latency: " << overall_validation.latency_improvement << "x"
                   << ", CPU: " << overall_validation.cpu_improvement << "x";
}
```

#### Phase 5 Success Criteria
- [ ] **Feature Flag System**: Seamless switching between legacy and separated architectures
- [ ] **Migration Safety**: Zero data loss during architecture transitions
- [ ] **Automatic Rollback**: Performance degradation triggers automatic rollback within 2 seconds
- [ ] **Backward Compatibility**: 100% API compatibility with existing TensorStore interfaces
- [ ] **Performance Targets**: All phase targets achieved (callback <25μs, throughput ≥80 MB/s, CPU <30%)
- [ ] **Production Readiness**: 99.9%+ uptime, <0.1% crash rate, zero memory leaks
- [ ] **Overall Improvement**: ≥2x throughput, ≥5x latency improvement, ≥2.5x CPU efficiency
- [ ] **Deployment Ready**: Container compatible, cloud ready, monitoring integrated

---

## Success Metrics and Validation

### Performance Targets

| Metric | Current (Mixed) | Target (Separated) | Improvement |
|--------|----------------|-------------------|-------------|
| **UCX Callback Latency** | 200-500μs | <25μs | 10-20x faster |
| **Network Progress Thread CPU** | 25-40% | <5% | 80%+ reduction |
| **Error Isolation** | Shared failure | Independent | 100% isolation |
| **Application Throughput** | 50-80 MB/s | 100-150 MB/s | 2-3x improvement |
| **Memory Usage** | Shared buffers | Separate pools | Better isolation |

### Validation Tests

#### 1. **Callback Latency Test**
```cpp
// Measure time spent in UCX callbacks
auto start = std::chrono::high_resolution_clock::now();
// UCX callback execution
auto end = std::chrono::high_resolution_clock::now();
EXPECT_LT(std::chrono::duration_cast<std::chrono::microseconds>(end - start), 
          std::chrono::microseconds(25));
```

#### 2. **Thread Isolation Test**
```cpp
// Ensure application work doesn't block network progress
InjectSlowApplicationWork(100ms);
auto network_response_time = MeasureNetworkResponseTime();
EXPECT_LT(network_response_time, 50us);  // Network unaffected by slow app work
```

#### 3. **Error Isolation Test**
```cpp
// Application errors shouldn't crash network layer
InjectApplicationException();
EXPECT_TRUE(NetworkLayerStillResponsive());
EXPECT_TRUE(CanEstablishNewConnections());
```

#### 4. **Resource Isolation Test**
```cpp
// Separate resource pools
EXPECT_NE(GetNetworkBufferPool(), GetApplicationBufferPool());
EXPECT_NE(GetNetworkThreads(), GetApplicationThreads());
```

---

## Risk Mitigation and Rollback Plan

### Risk Assessment
1. **Performance Regression**: New architecture could be slower initially
2. **Compatibility Issues**: Changes might break existing functionality  
3. **Complexity Increase**: More moving parts could introduce bugs
4. **Migration Failures**: Switching between architectures could fail

### Mitigation Strategies

#### 1. **Feature Flag Rollback**
```cpp
// Immediate rollback capability
if (PerformanceRegression() || FunctionalityBroken()) {
    feature_flags::ArchitectureMigration::SetMode(
        feature_flags::ArchitectureMode::LEGACY_MIXED);
    ABSL_LOG(WARNING) << "Rolled back to legacy architecture due to issues";
}
```

#### 2. **Gradual Migration**
- **Phase 1**: Network events only (no application changes)
- **Phase 2**: Message handling separation (application logic)
- **Phase 3**: Full separation with performance optimization
- **Phase 4**: Legacy code removal (only after proven stable)

#### 3. **Automated Testing**
- Continuous integration tests for both architectures
- Performance regression detection
- Automatic rollback on test failures

#### 4. **Monitoring and Alerting**
- Real-time performance monitoring
- Error rate tracking for each layer
- Automatic alerts for architectural violations

---

## Implementation Timeline Summary

| Phase | Duration | Focus | Deliverables |
|-------|----------|-------|--------------|
| **Phase 1** | Week 1-2 | Network Abstraction | Pure UCX layer, network events |
| **Phase 2** | Week 3-4 | Application Layer | Message handler, storage separation |
| **Phase 3** | Week 5-6 | Integration | Bridge layer, clean interfaces |
| **Phase 4** | Week 7-8 | Performance | Lock-free processing, optimization |
| **Phase 5** | Week 9-10 | Migration | Feature flags, testing, deployment |

**Total Duration**: 10 weeks

**Expected Outcome**: Clean architectural separation with 10-20x faster network callbacks, improved error isolation, and 2-3x overall performance improvement while maintaining full backward compatibility.

This plan addresses the fundamental architectural problem where UCX is inappropriately handling application concerns, creating a clean separation that will enable much higher performance and better maintainability.