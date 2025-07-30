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

#ifndef TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_POOL_H_
#define TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_POOL_H_

#include <memory>
#include <vector>
#include <atomic>
#include <cstddef>
#include "absl/synchronization/mutex.h"

namespace tensorstore {

/// PHASE 4: UCX Memory Pool for zero-copy buffer management
class UcxMemoryPool {
 public:
  /// Configuration for memory pool sizes
  struct Config {
    size_t small_buffer_size = 1024;      // 1KB buffers
    size_t medium_buffer_size = 16384;    // 16KB buffers  
    size_t large_buffer_size = 65536;     // 64KB buffers
    
    size_t small_pool_size = 100;         // Number of small buffers
    size_t medium_pool_size = 50;         // Number of medium buffers
    size_t large_pool_size = 20;          // Number of large buffers
    
    Config() = default;
  };
  
  /// Get singleton instance
  static UcxMemoryPool& Instance();
  
  /// Pooled buffer with atomic usage tracking
  struct PooledBuffer {
    char* data;
    size_t size;
    std::atomic<bool> in_use{false};
    
    explicit PooledBuffer(size_t sz) : data(new char[sz]), size(sz) {}
    ~PooledBuffer() { delete[] data; }
    
    // Non-copyable, non-movable
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;
  };
  
  /// RAII buffer lease that automatically returns buffer to pool
  class BufferLease {
   public:
    BufferLease(char* buffer_data, size_t buffer_size, PooledBuffer* source) 
      : data_(buffer_data), size_(buffer_size), source_buffer_(source) {}
    
    ~BufferLease() {
      if (source_buffer_) {
        source_buffer_->in_use.store(false, std::memory_order_release);
      }
    }
    
    // Non-copyable, movable
    BufferLease(const BufferLease&) = delete;
    BufferLease& operator=(const BufferLease&) = delete;
    BufferLease(BufferLease&& other) noexcept 
      : data_(other.data_), size_(other.size_), source_buffer_(other.source_buffer_) {
      other.source_buffer_ = nullptr;
    }
    BufferLease& operator=(BufferLease&& other) noexcept {
      if (this != &other) {
        // Return current buffer to pool
        if (source_buffer_) {
          source_buffer_->in_use.store(false, std::memory_order_release);
        }
        // Take ownership of other's buffer
        data_ = other.data_;
        size_ = other.size_;
        source_buffer_ = other.source_buffer_;
        other.source_buffer_ = nullptr;
      }
      return *this;
    }
    
    char* data() const { return data_; }
    size_t size() const { return size_; }
    
   private:
    char* data_;
    size_t size_;
    PooledBuffer* source_buffer_;
  };
  
  /// Acquire buffer from appropriate pool based on size
  std::unique_ptr<BufferLease> AcquireBuffer(size_t required_size);
  
  /// Initialize memory pools with default configuration
  void Initialize();
  
  /// Initialize memory pools with given configuration
  void Initialize(const Config& config);
  
  /// Get pool statistics for monitoring
  struct PoolStats {
    size_t small_pool_size;
    size_t medium_pool_size;
    size_t large_pool_size;
    size_t small_in_use;
    size_t medium_in_use;
    size_t large_in_use;
    size_t total_allocations;
    size_t pool_hits;
    size_t pool_misses;
  };
  PoolStats GetStats() const;
  
 private:
  UcxMemoryPool() = default;
  ~UcxMemoryPool() = default;
  
  /// Select appropriate pool based on required size
  std::vector<std::unique_ptr<PooledBuffer>>& SelectPool(size_t required_size);
  const std::vector<std::unique_ptr<PooledBuffer>>& SelectPool(size_t required_size) const;
  
  mutable absl::Mutex mutex_;
  bool initialized_ ABSL_GUARDED_BY(mutex_) = false;
  Config config_ ABSL_GUARDED_BY(mutex_);
  
  std::vector<std::unique_ptr<PooledBuffer>> small_buffers_ ABSL_GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<PooledBuffer>> medium_buffers_ ABSL_GUARDED_BY(mutex_);
  std::vector<std::unique_ptr<PooledBuffer>> large_buffers_ ABSL_GUARDED_BY(mutex_);
  
  // Statistics
  mutable std::atomic<size_t> total_allocations_{0};
  mutable std::atomic<size_t> pool_hits_{0};
  mutable std::atomic<size_t> pool_misses_{0};
};

}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REMOTE_DRAM_UCX_MEMORY_POOL_H_