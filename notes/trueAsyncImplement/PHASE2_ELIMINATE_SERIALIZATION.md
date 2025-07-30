# Phase 2: Eliminate Serialization Bottlenecks

## Problem Analysis
**Current Issue**: All UCX operations serialized through single worker and promise/future tracking
- **Single UCX Worker**: Only one worker handles all client connections
- **Mutex-Protected Operations**: Request ID generation, operation registration, completion
- **Callback Dispatcher Overhead**: Additional queuing and threading layers

## Current Serialization Points

### 2.1: Single UCX Worker Bottleneck
```cpp
// All operations go through ONE worker
ucp_worker_h worker_;  // Single worker for everything
absl::MutexLock lock(&mutex_);  // Serializes access
ucp_worker_progress(worker_);   // Single-threaded progress
```

### 2.2: Promise/Future Registration Serialization
```cpp
void UcxManager::RegisterPendingOperation(uint64_t request_id, Promise<void> promise, MessageType type) {
  absl::MutexLock lock(&mutex_);  // SERIALIZATION!
  auto op = std::make_unique<PendingWriteOperation>(request_id, std::move(promise));
  pending_write_operations_[request_id] = std::move(op);
}
```

### 2.3: Request ID Generation Serialization
```cpp
uint64_t UcxManager::GenerateRequestId() {
  absl::MutexLock lock(&mutex_);  // SERIALIZATION!
  return next_request_id_++;
}
```

## Phase 2 Solution: Per-Client UCX Workers

### Goals
1. **Create multiple UCX workers** (one per client endpoint)
2. **Lock-free operation tracking** using atomic operations
3. **Eliminate callback dispatcher overhead**
4. **Maintain write operation independence** (chunk1 doesn't wait for chunk0)

### Architecture Change

#### Step 2.1: Multiple UCX Workers
```cpp
class UcxManager {
private:
  struct ClientWorkerContext {
    ucp_worker_h worker;
    ucp_ep_h endpoint;
    std::thread progress_thread;
    std::atomic<bool> active{true};
    
    // Per-worker operation tracking (lock-free)
    std::atomic<uint64_t> next_request_id{1};
    absl::flat_hash_map<uint64_t, std::unique_ptr<PendingOperation>> pending_ops;
    absl::Mutex pending_ops_mutex;  // Minimal locking scope
  };
  
  std::vector<std::unique_ptr<ClientWorkerContext>> client_workers_;
  std::atomic<size_t> next_worker_index_{0};
  
public:
  // Round-robin worker assignment for load balancing
  ClientWorkerContext* GetWorkerForNewOperation() {
    size_t index = next_worker_index_.fetch_add(1) % client_workers_.size();
    return client_workers_[index].get();
  }
};
```

#### Step 2.2: Lock-Free Request ID Generation
```cpp
class ClientWorkerContext {
private:
  std::atomic<uint64_t> next_request_id_{1};
  
public:
  uint64_t GenerateRequestId() {
    return next_request_id_.fetch_add(1, std::memory_order_relaxed);  // NO MUTEX!
  }
};
```

#### Step 2.3: Per-Worker Progress Threads
```cpp
void ClientWorkerContext::WorkerProgressTask() {
  while (active.load(std::memory_order_acquire)) {
    ucp_worker_progress(worker);  // No global mutex needed
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
}
```

#### Step 2.4: Direct Callback Handling (No Dispatcher)
```cpp
void SendCallback(void* request, ucs_status_t status, void* user_data) {
  auto* context = static_cast<CallbackContext*>(user_data);
  
  // Direct completion - no dispatcher queue
  context->worker_ctx->CompletePendingOperation(context->request_id, 
      status == UCS_OK ? absl::OkStatus() : 
      absl::InternalError(ucs_status_string(status)));
  
  delete context;
  ucp_request_free(request);
}
```

### Write Operation Independence

#### Current Problem: Sequential Writes
```cpp
// Current: Each write waits for response
auto future1 = kvstore.Write("chunk0", data0);  // Blocks until server ACK
auto future2 = kvstore.Write("chunk1", data1);  // Can't start until chunk0 completes
```

#### Phase 2 Solution: Parallel Writes
```cpp
// After Phase 2: True parallel writes
auto future1 = kvstore.Write("chunk0", data0);  // Starts immediately
auto future2 = kvstore.Write("chunk1", data1);  // Starts immediately (different worker)
auto future3 = kvstore.Write("chunk2", data2);  // Starts immediately (different worker)

// All three operations proceed in parallel through UCX
```

### Implementation Strategy

#### Step 2.1: Create Worker Pool Architecture
1. **Initialize multiple UCX workers** at startup
2. **One progress thread per worker** for true parallelism
3. **Round-robin assignment** of operations to workers

#### Step 2.2: Eliminate Global Serialization
1. **Per-worker request ID generation** (atomic)
2. **Per-worker operation tracking** (minimal locking scope)
3. **Direct callback completion** (no dispatcher)

#### Step 2.3: Load Balancing Strategy
```cpp
ClientWorkerContext* UcxManager::SelectWorkerForWrite(const kvstore::Key& key) {
  // Option 1: Round-robin (simple load balancing)
  size_t index = next_worker_index_.fetch_add(1) % client_workers_.size();
  
  // Option 2: Key-based sharding (better cache locality)
  // size_t index = std::hash<std::string>{}(key) % client_workers_.size();
  
  return client_workers_[index].get();
}
```

### Expected Performance Impact
- **Concurrency**: True parallel operations (no false serialization)
- **Latency**: Eliminate mutex contention overhead
- **Throughput**: Linear scaling with number of workers
- **Memory**: Moderate increase (multiple worker contexts)

### Implementation Steps
1. **Step 2.1**: Create multi-worker architecture
2. **Step 2.2**: Implement per-worker progress threads
3. **Step 2.3**: Remove global serialization points
4. **Step 2.4**: Add load balancing for worker selection
5. **Step 2.5**: Remove callback dispatcher

### Questions for User
1. **Worker Pool Size**: How many UCX workers should we create? (suggestion: 4-8 workers)
2. **Load Balancing**: Round-robin or key-based sharding for worker selection?
3. **Memory vs Performance**: Are you okay with multiple worker contexts for better concurrency?

### Critical for Your Requirement
This phase is **essential** for your main concern:
> "Write operation for chunk1 should not wait for chunk0 to complete"

Phase 2 enables true independence between write operations by:
- Using separate UCX workers for different chunks
- Eliminating global serialization bottlenecks
- Allowing parallel UCX progress on multiple workers

## Files to Modify
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.h` (multi-worker architecture)
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:935-1563` (UcxManager class)
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:1650-1773` (WriteRemote method)

## Testing Strategy
- Test concurrent writes to different keys
- Verify operations complete independently
- Measure throughput scaling with worker count