# Phase 1: UCX Progress Polling Optimization

## Problem Analysis
**Current Issue**: Single progress thread with 100μs sleep creates severe latency bottleneck
- Location: `remote_dram_kvstore.cc:1465`
- Impact: Limits to ~10,000 polling cycles/second
- Result: All UCX operations effectively serialized

## Current Implementation
```cpp
void UcxManager::WorkerProgressTask() {
  while (true) {
    {
      absl::MutexLock lock(&mutex_);  // SERIALIZATION BOTTLENECK
      if (!initialized_ || !progress_task_running_) {
        break;
      }
      ucp_worker_progress(worker_);   // SINGLE WORKER
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));  // LATENCY BOTTLENECK
  }
}
```

## Phase 1 Solution: High-Frequency Polling

### Goals
1. **Reduce polling latency from 100μs to 1μs**
2. **Eliminate mutex contention on progress calls**
3. **Maintain distributed DRAM simulation constraints**
4. **Keep existing UCX single-worker architecture** (for now)

### Implementation Strategy

#### Step 1.1: Reduce Sleep Duration
```cpp
// Before (BAD):
std::this_thread::sleep_for(std::chrono::microseconds(100));

// After (BETTER):
std::this_thread::sleep_for(std::chrono::microseconds(1));
```

#### Step 1.2: Eliminate Progress Mutex (Lock-Free Progress)
```cpp
class UcxManager {
private:
  std::atomic<ucp_worker_h> worker_atomic_{nullptr};
  std::atomic<bool> progress_enabled_{false};
  
public:
  void WorkerProgressTask() {
    while (progress_enabled_.load(std::memory_order_acquire)) {
      ucp_worker_h current_worker = worker_atomic_.load(std::memory_order_acquire);
      if (current_worker) {
        ucp_worker_progress(current_worker);  // NO MUTEX!
      }
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
  }
};
```

#### Step 1.3: Optional - Busy Polling for Ultra-Low Latency
```cpp
void UcxManager::WorkerProgressTask() {
  int idle_count = 0;
  while (progress_enabled_.load(std::memory_order_acquire)) {
    ucp_worker_h current_worker = worker_atomic_.load(std::memory_order_acquire);
    if (current_worker) {
      unsigned progress_made = ucp_worker_progress(current_worker);
      
      if (progress_made > 0) {
        idle_count = 0;  // Reset idle counter
      } else {
        idle_count++;
        // Adaptive polling: busy wait initially, then micro-sleep
        if (idle_count > 1000) {
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        } else if (idle_count > 100) {
          std::this_thread::yield();
        }
        // else: busy poll
      }
    }
  }
}
```

### Expected Performance Impact
- **Latency Reduction**: 100x improvement (100μs → 1μs)
- **Throughput Increase**: 10-50x for small operations
- **CPU Usage**: Moderate increase due to higher polling frequency

### Implementation Steps
1. **Step 1.1**: Change sleep from 100μs to 1μs
2. **Step 1.2**: Replace mutex with atomic worker handle
3. **Step 1.3**: Test performance with busy polling option
4. **Step 1.4**: Add CPU affinity for progress thread

### Questions for User
1. **CPU Usage vs Performance**: Are you okay with higher CPU usage from 1μs polling or busy polling?
2. **Progress Thread Affinity**: Should we pin the progress thread to a dedicated CPU core?
3. **Adaptive Polling**: Should we implement adaptive polling that scales based on activity?

### Next Phase Dependencies
- Phase 1 provides foundation for Phase 2 (multiple workers)
- Removes primary latency bottleneck before addressing concurrency
- Essential for achieving true async behavior

## Files to Modify
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc:1450-1469`
- `tensorstore/kvstore/remote_dram/remote_dram_kvstore.h` (atomic members)

## Testing Strategy
- Measure latency improvement with simple ping-pong test
- Monitor CPU usage impact
- Verify no regression in correctness