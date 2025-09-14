#pragma once

#include "remote_dram_kvstore.h"
#include <memory>
#include <optional>
#include <atomic>

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