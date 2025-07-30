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

#ifndef TENSORSTORE_KVSTORE_REMOTE_DRAM_ZERO_COPY_BUILDER_H_
#define TENSORSTORE_KVSTORE_REMOTE_DRAM_ZERO_COPY_BUILDER_H_

#include <memory>
#include <cstddef>
#include <cstring>
#include "tensorstore/kvstore/key_range.h"
#include "absl/strings/cord.h"
#include "ucx_memory_pool.h"
#include "remote_dram_kvstore.h"

namespace tensorstore {

/// PHASE 4: Zero-copy message builder for UCX operations
class ZeroCopyMessageBuilder {
 public:
  /// Message buffer with managed memory lease
  struct MessageBuffer {
    std::unique_ptr<UcxMemoryPool::BufferLease> buffer_lease;
    MessageHeader* header;
    char* payload_start;
    size_t total_size;
    
    MessageBuffer(std::unique_ptr<UcxMemoryPool::BufferLease> lease,
                  MessageHeader* hdr, char* payload, size_t size)
      : buffer_lease(std::move(lease)), header(hdr), payload_start(payload), total_size(size) {}
    
    // Non-copyable, movable
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    MessageBuffer(MessageBuffer&&) = default;
    MessageBuffer& operator=(MessageBuffer&&) = default;
  };
  
  /// Build write message directly in UCX buffer (eliminates intermediate copies)
  static std::unique_ptr<MessageBuffer> BuildWriteMessage(
      const kvstore::Key& key, const absl::Cord& value, uint64_t request_id);
  
  /// Build read request message directly in UCX buffer
  static std::unique_ptr<MessageBuffer> BuildReadMessage(
      const kvstore::Key& key, uint64_t request_id);
  
 private:
  /// Copy absl::Cord data directly to buffer (single copy)
  static void CopyCordToBuffer(const absl::Cord& cord, char* dest_buffer);
};

/// PHASE 4: Zero-copy callback user data
struct ZeroCopyUserData {
  uint64_t request_id;
  std::unique_ptr<ZeroCopyMessageBuilder::MessageBuffer> message_buffer;
  ClientWorkerContext* worker_context;
  
  ZeroCopyUserData(uint64_t id, 
                   std::unique_ptr<ZeroCopyMessageBuilder::MessageBuffer> msg,
                   ClientWorkerContext* ctx)
    : request_id(id), message_buffer(std::move(msg)), worker_context(ctx) {}
};

/// Zero-copy callback for UCX send completion
void ZeroCopyWriteCallback(void* request, ucs_status_t status, void* user_data);

}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REMOTE_DRAM_ZERO_COPY_BUILDER_H_