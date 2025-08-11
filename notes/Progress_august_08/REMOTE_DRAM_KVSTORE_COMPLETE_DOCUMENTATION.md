# Remote DRAM KVStore Complete Documentation
## A Comprehensive Guide for TensorStore's UCX-based Distributed Memory Backend
### Version 1.0 - August 8, 2025

---

## Table of Contents
1. [Introduction and Concepts](#introduction-and-concepts)
2. [Architectural Overview](#architectural-overview)
3. [Core Components](#core-components)
4. [Execution Flow](#execution-flow)
5. [UCX Data Transfer Mechanism](#ucx-data-transfer-mechanism)
6. [TensorStore Integration](#tensorstore-integration)
7. [Buffering Strategy](#buffering-strategy)
8. [Concurrency and Asynchronous Operations](#concurrency-and-asynchronous-operations)
9. [Configuration and Setup](#configuration-and-setup)
10. [Visual Sequence Diagrams](#visual-sequence-diagrams)
11. [Complete Function Reference](#complete-function-reference)
12. [Performance Characteristics](#performance-characteristics)
13. [Troubleshooting Guide](#troubleshooting-guide)

---

## 1. Introduction and Concepts

### What is Remote DRAM KVStore?
Remote DRAM KVStore is a distributed key-value storage backend for TensorStore that enables direct memory-to-memory data transfer between separate processes or machines. It simulates remote DRAM access where a client process can store and retrieve data from a server process's memory (DRAM) without using traditional disk-based storage.

### What is UCX?
**UCX (Unified Communication X)** is a high-performance communication framework designed for HPC (High Performance Computing) and data-centric applications. It provides:
- **Zero-copy transfers**: Data moves directly from source to destination memory
- **Multiple transport layers**: InfiniBand, TCP/IP, shared memory
- **Asynchronous operations**: Non-blocking communication primitives
- **Hardware offload**: Leverages RDMA and other hardware acceleration

### What is TensorStore?
**TensorStore** is Google's library for reading and writing large multi-dimensional arrays. It provides:
- Unified API for various storage backends
- Support for zarr, n5, and other formats
- Asynchronous I/O operations
- Chunked and compressed storage

### Key Terminology
- **KVStore**: Key-Value Store - a database paradigm where data is stored as key-value pairs
- **DRAM**: Dynamic Random-Access Memory - the main memory of a computer
- **Endpoint**: A UCX communication channel between two processes
- **Worker**: UCX thread that processes communication events
- **Cord**: Google's efficient string representation (absl::Cord)
- **Future/Promise**: Asynchronous programming primitives for deferred computation

---

## 2. Architectural Overview

### System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     TensorStore Application                      │
├─────────────────────────────────────────────────────────────────┤
│                    TensorStore KVStore API                       │
│                 Future<ReadResult> / Future<Write>               │
├─────────────────────────────────────────────────────────────────┤
│                   RemoteDramDriver (Client)                      │
│                  ┌─────────────────────────┐                    │
│                  │  Async Operations       │                    │
│                  │  Request Generation     │                    │
│                  │  Future Management      │                    │
│                  └─────────────────────────┘                    │
├─────────────────────────────────────────────────────────────────┤
│                        UcxManager                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐         │
│  │ UCX Context  │  │ UCX Worker   │  │ Connection   │         │
│  │ & Config     │  │ Thread       │  │ Management   │         │
│  └──────────────┘  └──────────────┘  └──────────────┘         │
├─────────────────────────────────────────────────────────────────┤
│                    UCX Communication Layer                       │
│              (Tagged Messaging, Zero-Copy, RDMA)                 │
└─────────────────────────────────────────────────────────────────┘
                              ↕ Network ↕
┌─────────────────────────────────────────────────────────────────┐
│                        Server Process                            │
├─────────────────────────────────────────────────────────────────┤
│                   RemoteDramDriver (Server)                      │
│                  ┌─────────────────────────┐                    │
│                  │  RemoteDramStorage      │                    │
│                  │  (In-Memory HashMap)    │                    │
│                  └─────────────────────────┘                    │
├─────────────────────────────────────────────────────────────────┤
│                 UcxCallbackDispatcher                            │
│         (Multi-threaded callback processing)                     │
├─────────────────────────────────────────────────────────────────┤
│            Server UCX Listener & Worker                          │
└─────────────────────────────────────────────────────────────────┘
```

### Design Principles
1. **Separation of Concerns**: Client and server run as independent processes
2. **Asynchronous by Default**: All operations return futures immediately
3. **Zero-Copy Where Possible**: Minimize data copying for performance
4. **Thread Safety**: All shared resources protected by mutexes
5. **Resource Management**: RAII and reference counting for automatic cleanup

---

## 3. Core Components

### 3.1 UcxManager (Singleton)
**Purpose**: Manages all UCX resources and coordinates communication

**Key Responsibilities**:
- Initialize UCX context and worker
- Manage client/server endpoints
- Handle connection lifecycle
- Process UCX events
- Track pending operations

**Location**: `tensorstore/kvstore/remote_dram/remote_dram_kvstore.h:204-356`

### 3.2 RemoteDramStorage
**Purpose**: Server-side in-memory storage

**Key Features**:
- Thread-safe HashMap implementation
- Stores data as `absl::Cord` for efficiency
- No automatic eviction (data persists until shutdown)

**Location**: `tensorstore/kvstore/remote_dram/remote_dram_kvstore.h:177-201`

### 3.3 UcxCallbackDispatcher
**Purpose**: Processes UCX callbacks in separate threads to avoid deadlocks

**Architecture**:
- 2 worker threads (`NUM_WORKER_THREADS`)
- Round-robin task distribution
- Batch processing (4 callbacks per batch)

**Location**: `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:78-216`

### 3.4 AsyncOperationManager
**Purpose**: Controls concurrency and prevents system overload

**Limits**:
- Max 32 concurrent reads
- Max 64 concurrent writes
- Throttling with retry mechanism

**Location**: `tensorstore/kvstore/remote_dram/remote_dram_kvstore.h:127-173`

### 3.5 UcxNativeBuffer
**Purpose**: UCX-registered memory for zero-copy transfers

**Features**:
- Memory registration with UCX
- Automatic cleanup on destruction
- Move-only semantics

**Location**: `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:220-300`

### 3.6 Message Structures

```cpp
// Base header for all messages
struct MessageHeader {
  uint32_t magic_number;   // 0xDEADBEEF for validation
  MessageType type;        // WRITE_REQUEST, READ_REQUEST, etc.
  uint32_t key_length;     // Length of key string
  uint32_t value_length;   // Length of value data
  uint64_t request_id;     // Unique operation identifier
  uint32_t checksum;       // Currently disabled (always 0)
}

// Message types
enum class MessageType {
  WRITE_REQUEST = 1,   // Client → Server: Store data
  WRITE_RESPONSE = 2,  // Server → Client: Write acknowledgment
  READ_REQUEST = 3,    // Client → Server: Retrieve data
  READ_RESPONSE = 4    // Server → Client: Return data
}
```

---

## 4. Execution Flow

### 4.1 Initialization Flow

```
1. Application creates KVStore spec with remote_dram driver
   ↓
2. RemoteDramDriverSpec::DoOpen() called
   ↓
3. UcxManager::Initialize() - Setup UCX context
   ↓
4. Server Mode:                    Client Mode:
   - CreateListener()               - CreateClientEndpoint()
   - Post receive buffers           - Connect to server
   - Start accepting connections    - Wait for connection
   ↓
5. StartWorkerProgressTask() - Begin UCX event processing
   ↓
6. System ready for operations
```

### 4.2 Client-Server Connection

```
Client                              Server
  │                                   │
  ├── ucp_ep_create() ────────────→   │
  │                                   ├── UcxListenerCallback()
  │                                   ├── Accept connection
  │                                   ├── Create server endpoint
  │   ←──── Connection Established ───┤
  │                                   │
  └── Begin data transfer ────────→   └── Post receive buffers
```

---

## 5. UCX Data Transfer Mechanism

### 5.1 UCX Communication Model

UCX uses **Tagged Messaging** for data transfer:
- Each message has a unique tag for identification
- Non-blocking send/receive operations
- Callbacks triggered on completion

### 5.2 Tag Structure

```cpp
// Message tags for routing
constexpr ucp_tag_t UCX_TAG_WRITE_REQUEST  = 0x1000;
constexpr ucp_tag_t UCX_TAG_WRITE_RESPONSE = 0x1001;
constexpr ucp_tag_t UCX_TAG_READ_REQUEST   = 0x2000;
constexpr ucp_tag_t UCX_TAG_READ_RESPONSE  = 0x2001;
constexpr ucp_tag_t UCX_TAG_MASK          = 0xF000;
```

### 5.3 Data Transfer Steps

#### Send Operation:
```cpp
1. Prepare message buffer
2. Set up send parameters with callback
3. ucp_tag_send_nbx() - Non-blocking send
4. UCX handles transport (TCP/RDMA/SharedMem)
5. SendCallback() invoked on completion
6. Cleanup resources
```

#### Receive Operation:
```cpp
1. Allocate receive buffer
2. Set up receive parameters with callback
3. ucp_tag_recv_nbx() - Non-blocking receive
4. Data arrives in buffer
5. Receive callback processes data
6. Complete associated promise/future
```

### 5.4 Zero-Copy Optimizations

1. **Direct Cord Construction**: 
   ```cpp
   // Avoid intermediate string copies
   for (absl::string_view chunk : value.Chunks()) {
     std::memcpy(buffer + offset, chunk.data(), chunk.size());
   }
   ```

2. **Buffer Ownership Transfer**:
   ```cpp
   // Transfer buffer ownership to Cord
   result.value = absl::MakeCordFromExternal(
     absl::string_view(data, length),
     [buffer](absl::string_view) { delete[] buffer; }
   );
   ```

---

## 6. TensorStore Integration

### 6.1 Driver Registration

```cpp
// Register with TensorStore's driver system
namespace {
  const tensorstore::internal_kvstore::DriverRegistration<
      tensorstore::RemoteDramDriverSpec> registration;
}
```

### 6.2 KVStore API Implementation

#### Write Operation:
```cpp
Future<TimestampedStorageGeneration> Write(
    kvstore::Key key,
    std::optional<absl::Cord> value,
    kvstore::WriteOptions options)
```
- Converts tensor data to key-value pairs
- Returns future for async completion
- Supports partial writes via byte ranges

#### Read Operation:
```cpp
Future<kvstore::ReadResult> Read(
    kvstore::Key key,
    kvstore::ReadOptions options)
```
- Retrieves data by key
- Returns ReadResult with state (kValue/kMissing)
- Supports byte-range reads

### 6.3 Tensor Storage Format

TensorStore chunks tensors and stores them as:
- **Key**: Chunk coordinates encoded as string
- **Value**: Compressed/uncompressed chunk data as Cord
- **Metadata**: Stored in separate keys

Example key format: `"chunk/0/0/0"` for 3D tensor chunk at origin

---

## 7. Buffering Strategy

### 7.1 Client-Side Buffering

#### Write Path:
```
Application Data
    ↓
[Message Construction Buffer]
Size: header + key + value
Lifetime: Until send completes
    ↓
[Response Receive Buffer]
Size: 1KB
Lifetime: Until response received
```

#### Read Path:
```
[Request Buffer]
Size: header + key
Lifetime: Until send completes
    ↓
[Response Receive Buffer]
Size: 4MB + overhead
Lifetime: Can transfer to Cord
```

### 7.2 Server-Side Buffering

#### Adaptive Receive Buffers:
```cpp
// Posted proactively for incoming requests
Size: 4MB + header
Count: 5 maximum (kMaxConcurrentReceiveBuffers)
Type: UCX native registered memory
Reuse: Immediately reposted after processing
```

#### Response Buffers:
```cpp
// Allocated per response
Write Response: sizeof(WriteResponse) = ~64 bytes
Read Response: sizeof(ReadResponse) + value_size
Type: UCX native registered memory
Lifetime: Until send completes
```

### 7.3 Buffer Pool Management

```
┌─────────────────────────────────────┐
│      Server Receive Buffer Pool      │
├─────────────────────────────────────┤
│ Buffer 1: [4MB] - Active            │
│ Buffer 2: [4MB] - Active            │
│ Buffer 3: [4MB] - Active            │
│ Buffer 4: [4MB] - Processing        │
│ Buffer 5: [4MB] - Processing        │
├─────────────────────────────────────┤
│ Total: 20MB max server memory       │
└─────────────────────────────────────┘
```

---

## 8. Concurrency and Asynchronous Operations

### 8.1 Asynchronous Architecture

```
┌──────────────────────────────────────────┐
│          Application Thread               │
│  - Initiates operations                   │
│  - Returns futures immediately            │
└──────────────────────────────────────────┘
                    ↓
┌──────────────────────────────────────────┐
│         UCX Worker Thread                 │
│  - Polls for UCX events                   │
│  - Triggers callbacks                     │
│  - Non-blocking progress                  │
└──────────────────────────────────────────┘
                    ↓
┌──────────────────────────────────────────┐
│    Callback Dispatcher (2 threads)        │
│  - Process callbacks safely               │
│  - Complete promises                      │
│  - Round-robin distribution               │
└──────────────────────────────────────────┘
```

### 8.2 Concurrency Limits

| Component | Limit | Type | Reason |
|-----------|-------|------|--------|
| Server Receive Buffers | 5 | Hard | UCX stability |
| Callback Threads | 2 | Hard | Reduced for stability |
| Concurrent Reads | 32 | Soft | Throttling |
| Concurrent Writes | 64 | Soft | Throttling |
| Batch Size | 4 | Hard | Processing efficiency |

### 8.3 Promise/Future Pattern

```cpp
// Operation initiation
auto [promise, future] = PromiseFuturePair<ReadResult>::Make();
RegisterPendingOperation(request_id, std::move(promise));
SendRequest();
return future;  // Returns immediately

// Completion in callback
void Callback() {
  promise.SetResult(result);  // Completes future
}
```

### 8.4 Throttling Mechanism

```cpp
if (async_manager.ShouldThrottleReads()) {
  // Retry after delay
  std::thread([this]() {
    std::this_thread::sleep_for(100us);
    retry_operation();
  }).detach();
}
```

---

## 9. Configuration and Setup

### 9.1 JSON Configuration

#### Server Configuration:
```json
{
  "driver": "remote_dram",
  "listen_addr": "0.0.0.0:12345"
}
```

#### Client Configuration:
```json
{
  "driver": "remote_dram",
  "remote_addr": "192.168.1.100:12345"
}
```

### 9.2 UCX Parameters

```cpp
// Context parameters
ucp_params.features = UCP_FEATURE_TAG |      // Tagged messaging
                      UCP_FEATURE_WAKEUP |    // Event notification
                      UCP_FEATURE_AM |        // Active messages
                      UCP_FEATURE_RMA;        // Remote memory access

// Worker parameters
worker_params.thread_mode = UCS_THREAD_MODE_MULTI;  // Multi-threaded

// Memory registration
mem_params.flags = UCP_MEM_MAP_ALLOCATE;  // UCX allocates memory
```

### 9.3 Build Configuration

```python
# Bazel BUILD file
cc_library(
    name = "remote_dram_kvstore",
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:cord",
        "@ucx//:ucp",
        "//tensorstore/kvstore:kvstore",
    ],
)
```

### 9.4 Environment Variables

```bash
# UCX Configuration
export UCX_TLS=tcp,self        # Transport layers
export UCX_NET_DEVICES=eth0    # Network device
export UCX_LOG_LEVEL=info      # Logging level

# Performance tuning
export UCX_TCP_NODELAY=1       # Disable Nagle's algorithm
export UCX_MEMTYPE_CACHE=n     # Disable memory type cache
```

---

## 10. Visual Sequence Diagrams

### 10.1 Write Operation Sequence

```
Client                    UCX Layer                  Server
  │                           │                        │
  ├─1. WriteRemote()          │                        │
  ├─2. GenerateRequestId()    │                        │
  ├─3. BuildWriteMessage()    │                        │
  │   [Header|Key|Value]      │                        │
  ├─4. RegisterPending()      │                        │
  ├─5. PostWriteResponse()    │                        │
  │   Receive()               │                        │
  ├─6. ucp_tag_send_nbx()─────→                        │
  │                           │                        │
  │                           ├──7. Transport────────→ │
  │                           │                        ├─8. UcxNativeServerReceiveCallback()
  │                           │                        ├─9. Parse Message
  │                           │                        ├─10. Storage.Store(key, value)
  │                           │                        ├─11. BuildWriteResponse()
  │                           │←──12. ucp_tag_send_nbx─┤
  │                           │                        │
  │←──13. Transport────────────                        │
  ├─14. ClientWriteResponseCallback()                  │
  ├─15. CompletePending()      │                        │
  ├─16. promise.SetResult()    │                        │
  │                           │                        │
  └─17. Future completes      │                        │
```

### 10.2 Read Operation Sequence

```
Client                    UCX Layer                  Server
  │                           │                        │
  ├─1. ReadRemote()           │                        │
  ├─2. GenerateRequestId()    │                        │
  ├─3. BuildReadMessage()     │                        │
  │   [Header|Key]            │                        │
  ├─4. RegisterPendingRead()  │                        │
  ├─5. PostReadResponse()     │                        │
  │   Receive()               │                        │
  ├─6. ucp_tag_send_nbx()─────→                        │
  │                           │                        │
  │                           ├──7. Transport────────→ │
  │                           │                        ├─8. UcxNativeServerReceiveCallback()
  │                           │                        ├─9. Parse Message
  │                           │                        ├─10. value = Storage.Get(key)
  │                           │                        ├─11. BuildReadResponse()
  │                           │                        │    [Header|Value]
  │                           │←──12. ucp_tag_send_nbx─┤
  │                           │                        │
  │←──13. Transport────────────                        │
  ├─14. ClientReceiveCallback()│                        │
  ├─15. Parse Response        │                        │
  ├─16. Create ReadResult     │                        │
  ├─17. Transfer buffer       │                        │
  │    to Cord (zero-copy)    │                        │
  ├─18. CompletePendingRead() │                        │
  ├─19. promise.SetResult()   │                        │
  │                           │                        │
  └─20. Future completes      │                        │
```

### 10.3 Connection Establishment

```
Client                         Server
  │                              │
  ├─1. CreateClientEndpoint()    │
  ├─2. Parse address:port        │
  ├─3. ucp_ep_create()           │
  │                              │
  ├─4. Connection Request────────→
  │                              ├─5. UcxListenerCallback()
  │                              ├─6. ucp_ep_create()
  │                              ├─7. RegisterClientEndpoint()
  │                              ├─8. PostAdaptiveServerReceive()
  │                              │    (5 buffers)
  │←─9. Connection Accepted───────┤
  │                              │
  ├─10. Poll for establishment   │
  ├─11. ucp_ep_query()          │
  ├─12. Connection ready        │
  │                              │
  └─13. Begin operations────────→└─14. Process requests
```

---

## 11. Complete Function Reference

### 11.1 UcxManager Functions

| Function | Purpose | Parameters | Returns |
|----------|---------|------------|---------|
| `Initialize()` | Setup UCX context and worker | None | `absl::Status` |
| `CreateListener(addr)` | Start server listener | `string` address | `Result<ucp_listener_h>` |
| `CreateClientEndpoint(addr)` | Connect to server | `string` address | `Result<ucp_ep_h>` |
| `GenerateRequestId()` | Get unique request ID | None | `uint64_t` |
| `RegisterPendingOperation()` | Track write operation | `id`, `promise`, `type` | `void` |
| `RegisterPendingReadOperation()` | Track read operation | `id`, `promise` | `void` |
| `CompletePendingOperationDirect()` | Complete write | `id`, `status` | `void` |
| `CompletePendingReadOperationDirect()` | Complete read | `id`, `result` | `void` |
| `PostAdaptiveServerReceive()` | Post receive buffer | `bool` locked | `void` |
| `SendReadResponse()` | Send read result | `endpoint`, `id`, `value` | `void` |
| `SendWriteResponse()` | Send write ack | `endpoint`, `id`, `status` | `void` |
| `StartWorkerProgressTask()` | Start UCX polling | None | `void` |
| `WorkerProgressTask()` | UCX event loop | None | `void` |
| `Shutdown()` | Clean up resources | None | `void` |

### 11.2 RemoteDramStorage Functions

| Function | Purpose | Parameters | Returns |
|----------|---------|------------|---------|
| `Store(key, value)` | Store data | `string`, `Cord` | `void` |
| `Get(key)` | Retrieve data | `string` | `optional<Cord>` |
| `Exists(key)` | Check existence | `string` | `bool` |
| `Remove(key)` | Delete entry | `string` | `bool` |
| `GetAllKeys()` | List all keys | None | `vector<string>` |
| `GetKeyCount()` | Count entries | None | `size_t` |

### 11.3 RemoteDramDriver Functions

| Function | Purpose | Parameters | Returns |
|----------|---------|------------|---------|
| `Read()` | Read from store | `key`, `options` | `Future<ReadResult>` |
| `Write()` | Write to store | `key`, `value`, `options` | `Future<Generation>` |
| `ReadLocal()` | Server-side read | `key`, `options` | `Future<ReadResult>` |
| `ReadRemote()` | Client-side read | `key`, `options` | `Future<ReadResult>` |
| `WriteLocal()` | Server-side write | `key`, `value` | `Future<Generation>` |
| `WriteRemote()` | Client-side write | `key`, `value` | `Future<Generation>` |
| `PostReadResponseReceive()` | Setup read receive | `request_id` | `void` |
| `PostWriteResponseReceive()` | Setup write receive | `request_id` | `void` |

### 11.4 UcxCallbackDispatcher Functions

| Function | Purpose | Parameters | Returns |
|----------|---------|------------|---------|
| `Initialize()` | Start worker threads | None | `void` |
| `Shutdown()` | Stop workers | None | `void` |
| `ScheduleCallback()` | Queue callback | `function` | `void` |
| `WorkerLoop()` | Process callbacks | `thread_id` | `void` |

### 11.5 Callback Functions

| Function | Purpose | Called When |
|----------|---------|-------------|
| `SendCallback()` | Handle send completion | UCX send completes |
| `ClientReceiveCallback()` | Process read response | Read response arrives |
| `ClientWriteResponseCallback()` | Process write response | Write response arrives |
| `UcxNativeServerReceiveCallback()` | Handle server request | Request arrives at server |
| `UcxListenerCallback()` | Accept connections | Client connects |
| `UcxErrorHandler()` | Handle UCX errors | Connection error occurs |

### 11.6 Helper Functions

| Function | Purpose | Parameters | Returns |
|----------|---------|------------|---------|
| `ZeroCopyMessageBuilder::BuildWriteMessage()` | Create write message | `key`, `value`, `id` | `unique_ptr<char[]>` |
| `ZeroCopyMessageBuilder::BuildReadMessage()` | Create read message | `key`, `id` | `unique_ptr<char[]>` |
| `message_utils::InitializeMessageHeader()` | Setup header | `header`, `type`, etc. | `void` |
| `message_utils::VerifyMessageHeader()` | Validate message | `header`, `size` | `bool` |
| `message_utils::CalculateChecksum()` | Compute checksum | `data`, `size` | `uint32_t` |

---

## 12. Performance Characteristics

### 12.1 Latency Breakdown

| Operation Phase | Typical Latency | Factors |
|-----------------|-----------------|---------|
| Request Generation | <1 μs | Memory allocation |
| UCX Send | 10-50 μs | Network type |
| Network Transfer | 50-500 μs | Distance, bandwidth |
| Server Processing | 5-20 μs | Storage lookup |
| Response Transfer | 50-500 μs | Data size |
| Callback Processing | 5-10 μs | Thread scheduling |
| **Total Round-trip** | **120-1100 μs** | **End-to-end** |

### 12.2 Throughput Limits

```
Maximum Concurrent Operations:
┌──────────────────────────────┐
│ Reads:  32 (soft limit)      │
│ Writes: 64 (soft limit)      │
│ Server: 5 buffers (hard)     │
└──────────────────────────────┘

Theoretical Maximum:
- Small operations (1KB): ~10,000 ops/sec
- Medium operations (1MB): ~1,000 ops/sec  
- Large operations (10MB): ~100 ops/sec
```

### 12.3 Memory Usage

| Component | Memory Usage | Notes |
|-----------|--------------|-------|
| Server receive buffers | 20 MB | 5 × 4MB |
| Client buffers | Variable | Per operation |
| Storage overhead | ~1.5x data | HashMap + Cord |
| UCX overhead | ~10 MB | Context + worker |
| Callback queues | <1 MB | Pending operations |

### 12.4 Bottlenecks

1. **UCX Stability**: Limited to 5 concurrent server buffers
2. **Single Worker Thread**: Sequential event processing
3. **No Batching**: Each operation processed individually
4. **Memory Growth**: No automatic eviction

---

## 13. Troubleshooting Guide

### 13.1 Common Issues

#### Connection Failures
```
Error: "Failed to create UCX client endpoint"
Causes:
- Server not running
- Firewall blocking port
- Wrong IP address

Solution:
1. Verify server is listening: netstat -an | grep 12345
2. Check firewall: sudo ufw status
3. Test connectivity: nc -zv server_ip 12345
```

#### UCX Initialization Errors
```
Error: "Failed to initialize UCX context"
Causes:
- UCX not installed
- Wrong UCX version
- Missing permissions

Solution:
1. Install UCX: sudo apt-get install ucx
2. Check version: ucx_info -v
3. Set environment: export UCX_TLS=tcp,self
```

#### Memory Exhaustion
```
Symptom: Server crashes with OOM
Causes:
- No data eviction
- Too many large values
- Memory leaks

Solution:
1. Monitor memory: watch -n 1 free -h
2. Implement TTL/eviction
3. Restart server periodically
```

### 13.2 Debug Logging

```cpp
// Enable verbose logging
export GLOG_minloglevel=0
export UCX_LOG_LEVEL=debug

// Add custom logging
ABSL_LOG(INFO) << "Operation: " << request_id;
message_utils::LogMessageBuffer(buffer, size, "Debug");
```

### 13.3 Performance Tuning

```bash
# Increase receive buffers (risky)
kMaxConcurrentReceiveBuffers = 10;  // May crash

# Tune UCX parameters
export UCX_TCP_NODELAY=1
export UCX_TCP_SNDBUF=2097152
export UCX_TCP_RCVBUF=2097152

# CPU affinity
taskset -c 0-3 ./server  # Bind to cores 0-3
```

### 13.4 Monitoring

```cpp
// Add metrics collection
struct Metrics {
  std::atomic<uint64_t> total_reads{0};
  std::atomic<uint64_t> total_writes{0};
  std::atomic<uint64_t> bytes_transferred{0};
  std::chrono::steady_clock::time_point start_time;
  
  void Report() {
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto seconds = std::chrono::duration<double>(elapsed).count();
    
    std::cout << "Operations/sec: " 
              << (total_reads + total_writes) / seconds << "\n";
    std::cout << "Throughput MB/s: " 
              << bytes_transferred / (seconds * 1024 * 1024) << "\n";
  }
};
```

---

## Appendix A: Complete Build Instructions

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libnuma-dev \
  librdmacm-dev \
  libibverbs-dev

# 2. Build UCX from source
git clone https://github.com/openucx/ucx.git
cd ucx
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install

# 3. Build TensorStore with remote_dram
cd tensorstore
bazel build //tensorstore/kvstore/remote_dram:remote_dram_kvstore

# 4. Run tests
bazel test //tensorstore/kvstore/remote_dram:all

# 5. Start server
./bazel-bin/examples/two_nodes/node2_server

# 6. Run client
./bazel-bin/examples/two_nodes/node1_writer 127.0.0.1:12345
```

---

## Appendix B: Example Usage

### B.1 Python Client Example
```python
import tensorstore as ts
import asyncio

async def main():
    # Open remote DRAM store
    store = await ts.open({
        'driver': 'remote_dram',
        'remote_addr': '192.168.1.100:12345'
    })
    
    # Write tensor
    array = np.random.rand(1000, 1000)
    await store.write(array)
    
    # Read tensor
    result = await store.read()
    print(f"Read {result.shape} array")

asyncio.run(main())
```

### B.2 C++ Client Example
```cpp
#include "tensorstore/kvstore/kvstore.h"

int main() {
  // Create spec
  auto spec = tensorstore::kvstore::Spec::FromJson({
    {"driver", "remote_dram"},
    {"remote_addr", "127.0.0.1:12345"}
  });
  
  // Open store
  auto store = tensorstore::kvstore::Open(spec).result();
  
  // Write data
  auto write_future = store->Write("mykey", absl::Cord("myvalue"));
  write_future.Wait();
  
  // Read data
  auto read_future = store->Read("mykey");
  auto result = read_future.result();
  
  if (result.state == tensorstore::kvstore::ReadResult::kValue) {
    std::cout << "Value: " << result.value << std::endl;
  }
  
  return 0;
}
```

---

## Appendix C: Glossary

| Term | Definition |
|------|------------|
| **Cord** | Google's efficient rope data structure for strings |
| **Endpoint** | UCX communication channel between processes |
| **Future** | Placeholder for async operation result |
| **KVStore** | Key-Value Store interface |
| **Promise** | Object to set future's value |
| **RDMA** | Remote Direct Memory Access |
| **Tagged Messaging** | UCX message passing with tags |
| **UCX** | Unified Communication X framework |
| **Worker** | Thread processing UCX events |
| **Zero-copy** | Data transfer without intermediate copies |

---

## Document Version History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | Aug 8, 2025 | AI Assistant | Initial comprehensive documentation |

---

## Contact and Support

For questions, issues, or contributions:
- GitHub: [tensorstore/tensorstore](https://github.com/google/tensorstore)
- Documentation: [tensorstore.readthedocs.io](https://tensorstore.readthedocs.io)

---

*This documentation provides a complete understanding of the Remote DRAM KVStore implementation. For the latest updates, refer to the source code in `tensorstore/kvstore/remote_dram/`.*