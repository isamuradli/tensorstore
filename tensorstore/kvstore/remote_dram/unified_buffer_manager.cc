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