# Phase 3: Error Handling and Recovery Enhancement
## Priority: MEDIUM - Improves reliability and production readiness

### Overview
Enhance the current basic error handling with comprehensive error recovery, connection resilience, and graceful degradation mechanisms. The current implementation only logs errors without attempting recovery.

### Current State Analysis
**Existing Error Handling Limitations:**
- **Basic error handler:** `UcxErrorHandler` only logs errors (line 490)
- **No connection recovery:** Failed endpoints are not restored
- **No retry mechanisms:** Operations fail permanently on transient errors
- **Limited error classification:** All errors treated the same way
- **No graceful degradation:** System fails hard on any UCX error

### Goals
1. ✅ Implement intelligent error classification and recovery
2. ✅ Add automatic connection restoration for transient failures
3. ✅ Create retry mechanisms with exponential backoff
4. ✅ Establish graceful degradation patterns
5. ✅ Improve observability with comprehensive error metrics
6. ✅ Maintain backward compatibility with existing error handling

### Phase 3 Prerequisites
- ✅ Phase 1 (Worker Progress) completed and stable in production
- ✅ Phase 2 (Memory Management) completed and stable in production
- ✅ All previous phases showing no critical issues for 1 week

### Implementation Steps

#### Step 3.1: Create Error Classification System
**New File:** `error_classification.h`

```cpp
#pragma once
#include <ucp/api/ucp.h>
#include "absl/status/status.h"
#include <chrono>
#include <string>

namespace tensorstore {

enum class UCXErrorSeverity {
  TRANSIENT,      // Temporary issue, retry recommended
  RECOVERABLE,    // Connection issue, reconnection may help
  PERMANENT,      // Configuration or resource issue, retry unlikely to help
  CRITICAL        // System-level issue, requires intervention
};

enum class UCXErrorCategory {
  CONNECTION,     // Network connectivity issues
  RESOURCE,       // Memory, buffer, or system resource issues
  CONFIGURATION,  // Invalid parameters or setup issues
  PROTOCOL,       // UCX protocol or communication issues
  SYSTEM,         // Underlying system errors
  UNKNOWN         // Unclassified errors
};

struct UCXErrorInfo {
  ucs_status_t ucx_status;
  UCXErrorSeverity severity;
  UCXErrorCategory category;
  std::string description;
  std::chrono::milliseconds suggested_retry_delay;
  int max_retry_attempts;
  bool requires_reconnection;
  bool allows_graceful_degradation;

  // Convert to absl::Status with appropriate code
  absl::Status ToAbslStatus() const;
};

class UCXErrorClassifier {
public:
  static UCXErrorInfo ClassifyError(ucs_status_t status, const std::string& context = "");

  static bool IsRetryable(ucs_status_t status);
  static bool RequiresReconnection(ucs_status_t status);
  static std::chrono::milliseconds GetRetryDelay(ucs_status_t status, int attempt_number);

private:
  static UCXErrorInfo ClassifyConnectionError(ucs_status_t status);
  static UCXErrorInfo ClassifyResourceError(ucs_status_t status);
  static UCXErrorInfo ClassifyProtocolError(ucs_status_t status);
};

} // namespace tensorstore
```

#### Step 3.2: Implement Error Classification Logic
**New File:** `error_classification.cc`

```cpp
#include "error_classification.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"

namespace tensorstore {

UCXErrorInfo UCXErrorClassifier::ClassifyError(ucs_status_t status, const std::string& context) {
  switch (status) {
    // Transient network errors
    case UCS_ERR_UNREACHABLE:
    case UCS_ERR_CONNECTION_RESET:
    case UCS_ERR_TIMED_OUT:
      return {
        .ucx_status = status,
        .severity = UCXErrorSeverity::TRANSIENT,
        .category = UCXErrorCategory::CONNECTION,
        .description = absl::StrFormat("Transient connection error: %s in %s",
                                      ucs_status_string(status), context),
        .suggested_retry_delay = std::chrono::milliseconds(100),
        .max_retry_attempts = 5,
        .requires_reconnection = true,
        .allows_graceful_degradation = true
      };

    // Resource exhaustion errors
    case UCS_ERR_NO_MEMORY:
    case UCS_ERR_NO_RESOURCE:
    case UCS_ERR_BUSY:
      return {
        .ucx_status = status,
        .severity = UCXErrorSeverity::RECOVERABLE,
        .category = UCXErrorCategory::RESOURCE,
        .description = absl::StrFormat("Resource exhaustion: %s in %s",
                                      ucs_status_string(status), context),
        .suggested_retry_delay = std::chrono::milliseconds(500),
        .max_retry_attempts = 3,
        .requires_reconnection = false,
        .allows_graceful_degradation = true
      };

    // Configuration errors
    case UCS_ERR_INVALID_PARAM:
    case UCS_ERR_UNSUPPORTED:
      return {
        .ucx_status = status,
        .severity = UCXErrorSeverity::PERMANENT,
        .category = UCXErrorCategory::CONFIGURATION,
        .description = absl::StrFormat("Configuration error: %s in %s",
                                      ucs_status_string(status), context),
        .suggested_retry_delay = std::chrono::milliseconds(0),
        .max_retry_attempts = 0,
        .requires_reconnection = false,
        .allows_graceful_degradation = false
      };

    // Critical system errors
    case UCS_ERR_IO_ERROR:
    case UCS_ERR_CANCELED:
      return {
        .ucx_status = status,
        .severity = UCXErrorSeverity::CRITICAL,
        .category = UCXErrorCategory::SYSTEM,
        .description = absl::StrFormat("Critical system error: %s in %s",
                                      ucs_status_string(status), context),
        .suggested_retry_delay = std::chrono::milliseconds(1000),
        .max_retry_attempts = 1,
        .requires_reconnection = true,
        .allows_graceful_degradation = false
      };

    default:
      return {
        .ucx_status = status,
        .severity = UCXErrorSeverity::RECOVERABLE,
        .category = UCXErrorCategory::UNKNOWN,
        .description = absl::StrFormat("Unknown error: %s in %s",
                                      ucs_status_string(status), context),
        .suggested_retry_delay = std::chrono::milliseconds(200),
        .max_retry_attempts = 2,
        .requires_reconnection = false,
        .allows_graceful_degradation = true
      };
  }
}

bool UCXErrorClassifier::IsRetryable(ucs_status_t status) {
  auto error_info = ClassifyError(status);
  return error_info.max_retry_attempts > 0;
}

bool UCXErrorClassifier::RequiresReconnection(ucs_status_t status) {
  auto error_info = ClassifyError(status);
  return error_info.requires_reconnection;
}

std::chrono::milliseconds UCXErrorClassifier::GetRetryDelay(ucs_status_t status, int attempt_number) {
  auto error_info = ClassifyError(status);
  // Exponential backoff with jitter
  auto base_delay = error_info.suggested_retry_delay;
  auto exponential_delay = base_delay * (1 << attempt_number);  // 2^attempt_number
  auto jitter = std::chrono::milliseconds(rand() % 50);  // 0-50ms jitter
  return std::min(exponential_delay + jitter, std::chrono::seconds(10));  // Cap at 10 seconds
}

absl::Status UCXErrorInfo::ToAbslStatus() const {
  switch (severity) {
    case UCXErrorSeverity::TRANSIENT:
      return absl::UnavailableError(description);
    case UCXErrorSeverity::RECOVERABLE:
      return absl::ResourceExhaustedError(description);
    case UCXErrorSeverity::PERMANENT:
      return absl::InvalidArgumentError(description);
    case UCXErrorSeverity::CRITICAL:
      return absl::InternalError(description);
    default:
      return absl::UnknownError(description);
  }
}

} // namespace tensorstore
```

#### Step 3.3: Create Connection Recovery Manager
**New File:** `connection_recovery.h`

```cpp
#pragma once
#include "error_classification.h"
#include "remote_dram_kvstore.h"
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>

namespace tensorstore {

class ConnectionRecoveryManager {
public:
  enum class EndpointState {
    HEALTHY,
    DEGRADED,      // Experiencing errors but still functional
    RECOVERING,    // Attempting to recover
    FAILED         // Recovery failed, manual intervention needed
  };

  struct EndpointInfo {
    ucp_ep_h endpoint;
    std::string address;
    EndpointState state;
    std::atomic<int> error_count{0};
    std::atomic<int> recovery_attempts{0};
    std::chrono::steady_clock::time_point last_error;
    std::chrono::steady_clock::time_point last_recovery_attempt;
    std::mutex recovery_mutex;
  };

  struct RecoveryMetrics {
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> successful_recoveries{0};
    std::atomic<uint64_t> failed_recoveries{0};
    std::atomic<uint64_t> active_recoveries{0};
  };

  static ConnectionRecoveryManager& Instance();

  // Register endpoints for monitoring
  void RegisterEndpoint(ucp_ep_h endpoint, const std::string& address);
  void UnregisterEndpoint(ucp_ep_h endpoint);

  // Handle errors and trigger recovery if needed
  void HandleEndpointError(ucp_ep_h endpoint, ucs_status_t status, const std::string& context);

  // Check if endpoint is healthy for new operations
  bool IsEndpointHealthy(ucp_ep_h endpoint);

  // Get endpoint state and metrics
  EndpointState GetEndpointState(ucp_ep_h endpoint);
  const RecoveryMetrics& GetMetrics() const { return metrics_; }

  // Configuration
  void SetMaxRecoveryAttempts(int max_attempts) { max_recovery_attempts_ = max_attempts; }
  void SetErrorThreshold(int threshold) { error_threshold_ = threshold; }

private:
  ConnectionRecoveryManager() = default;

  std::unordered_map<ucp_ep_h, std::unique_ptr<EndpointInfo>> endpoints_;
  mutable std::shared_mutex endpoints_mutex_;
  RecoveryMetrics metrics_;

  std::atomic<int> max_recovery_attempts_{3};
  std::atomic<int> error_threshold_{5};  // Errors before marking as degraded

  // Recovery implementation
  void AttemptRecovery(EndpointInfo* endpoint_info);
  bool RecreateEndpoint(EndpointInfo* endpoint_info);
  void MarkEndpointFailed(EndpointInfo* endpoint_info);
};

} // namespace tensorstore
```

#### Step 3.4: Implement Connection Recovery Logic
**New File:** `connection_recovery.cc`

```cpp
#include "connection_recovery.h"
#include "absl/log/absl_log.h"
#include <thread>

namespace tensorstore {

ConnectionRecoveryManager& ConnectionRecoveryManager::Instance() {
  static ConnectionRecoveryManager instance;
  return instance;
}

void ConnectionRecoveryManager::RegisterEndpoint(ucp_ep_h endpoint, const std::string& address) {
  std::unique_lock lock(endpoints_mutex_);

  auto endpoint_info = std::make_unique<EndpointInfo>();
  endpoint_info->endpoint = endpoint;
  endpoint_info->address = address;
  endpoint_info->state = EndpointState::HEALTHY;
  endpoint_info->last_error = std::chrono::steady_clock::now();

  endpoints_[endpoint] = std::move(endpoint_info);

  ABSL_LOG(INFO) << "Registered endpoint for recovery monitoring: " << address;
}

void ConnectionRecoveryManager::UnregisterEndpoint(ucp_ep_h endpoint) {
  std::unique_lock lock(endpoints_mutex_);

  auto it = endpoints_.find(endpoint);
  if (it != endpoints_.end()) {
    ABSL_LOG(INFO) << "Unregistered endpoint: " << it->second->address;
    endpoints_.erase(it);
  }
}

void ConnectionRecoveryManager::HandleEndpointError(ucp_ep_h endpoint, ucs_status_t status,
                                                   const std::string& context) {
  metrics_.total_errors++;

  std::shared_lock lock(endpoints_mutex_);
  auto it = endpoints_.find(endpoint);
  if (it == endpoints_.end()) {
    ABSL_LOG(WARNING) << "Error on unregistered endpoint, ignoring";
    return;
  }

  auto* endpoint_info = it->second.get();
  lock.unlock();  // Release shared lock before potential recovery

  // Classify the error
  auto error_info = UCXErrorClassifier::ClassifyError(status, context);

  ABSL_LOG(ERROR) << "Endpoint error [" << endpoint_info->address << "]: "
                  << error_info.description
                  << " (severity: " << static_cast<int>(error_info.severity) << ")";

  // Update error tracking
  endpoint_info->error_count++;
  endpoint_info->last_error = std::chrono::steady_clock::now();

  // Determine if recovery is needed
  bool should_recover = false;

  if (error_info.severity == UCXErrorSeverity::CRITICAL) {
    // Critical errors always trigger recovery
    should_recover = true;
  } else if (error_info.requires_reconnection) {
    // Connection errors trigger recovery
    should_recover = true;
  } else if (endpoint_info->error_count >= error_threshold_) {
    // Too many errors, try recovery
    should_recover = true;
  }

  // Update endpoint state
  if (endpoint_info->error_count >= error_threshold_) {
    endpoint_info->state = EndpointState::DEGRADED;
  }

  // Attempt recovery if needed and possible
  if (should_recover && error_info.max_retry_attempts > 0) {
    if (endpoint_info->recovery_attempts < max_recovery_attempts_) {
      ABSL_LOG(INFO) << "Scheduling recovery for endpoint: " << endpoint_info->address;

      // Schedule recovery in a separate thread to avoid blocking
      std::thread([this, endpoint_info]() {
        AttemptRecovery(endpoint_info);
      }).detach();
    } else {
      ABSL_LOG(ERROR) << "Maximum recovery attempts exceeded for endpoint: "
                      << endpoint_info->address;
      MarkEndpointFailed(endpoint_info);
    }
  }
}

bool ConnectionRecoveryManager::IsEndpointHealthy(ucp_ep_h endpoint) {
  std::shared_lock lock(endpoints_mutex_);
  auto it = endpoints_.find(endpoint);
  if (it == endpoints_.end()) {
    return false;  // Unknown endpoint is not healthy
  }

  auto state = it->second->state;
  return state == EndpointState::HEALTHY || state == EndpointState::DEGRADED;
}

ConnectionRecoveryManager::EndpointState ConnectionRecoveryManager::GetEndpointState(ucp_ep_h endpoint) {
  std::shared_lock lock(endpoints_mutex_);
  auto it = endpoints_.find(endpoint);
  return (it != endpoints_.end()) ? it->second->state : EndpointState::FAILED;
}

void ConnectionRecoveryManager::AttemptRecovery(EndpointInfo* endpoint_info) {
  std::lock_guard<std::mutex> recovery_lock(endpoint_info->recovery_mutex);

  // Check if already recovered by another thread
  if (endpoint_info->state == EndpointState::HEALTHY) {
    return;
  }

  metrics_.active_recoveries++;
  endpoint_info->state = EndpointState::RECOVERING;
  endpoint_info->recovery_attempts++;
  endpoint_info->last_recovery_attempt = std::chrono::steady_clock::now();

  ABSL_LOG(INFO) << "Attempting recovery for endpoint: " << endpoint_info->address
                 << " (attempt " << endpoint_info->recovery_attempts << ")";

  // Wait before recovery attempt (exponential backoff)
  auto delay = UCXErrorClassifier::GetRetryDelay(UCS_ERR_CONNECTION_RESET,
                                                endpoint_info->recovery_attempts);
  std::this_thread::sleep_for(delay);

  // Attempt to recreate the endpoint
  bool recovery_successful = RecreateEndpoint(endpoint_info);

  metrics_.active_recoveries--;

  if (recovery_successful) {
    endpoint_info->state = EndpointState::HEALTHY;
    endpoint_info->error_count = 0;  // Reset error count on successful recovery
    metrics_.successful_recoveries++;

    ABSL_LOG(INFO) << "Successfully recovered endpoint: " << endpoint_info->address;
  } else {
    metrics_.failed_recoveries++;

    if (endpoint_info->recovery_attempts >= max_recovery_attempts_) {
      MarkEndpointFailed(endpoint_info);
    } else {
      // Will try again on next error
      endpoint_info->state = EndpointState::DEGRADED;
      ABSL_LOG(WARNING) << "Recovery failed for endpoint: " << endpoint_info->address
                        << ", will retry on next error";
    }
  }
}

bool ConnectionRecoveryManager::RecreateEndpoint(EndpointInfo* endpoint_info) {
  try {
    // Destroy the old endpoint
    if (endpoint_info->endpoint) {
      ucp_ep_destroy(endpoint_info->endpoint);
      endpoint_info->endpoint = nullptr;
    }

    // Create new endpoint using UcxManager
    auto& ucx_manager = UcxManager::Instance();
    auto new_endpoint_result = ucx_manager.CreateClientEndpoint(endpoint_info->address);

    if (new_endpoint_result.ok()) {
      endpoint_info->endpoint = *new_endpoint_result;

      // Update UcxManager's endpoint tracking
      ucx_manager.RegisterClientSideEndpoint(endpoint_info->endpoint);

      return true;
    } else {
      ABSL_LOG(ERROR) << "Failed to recreate endpoint: " << new_endpoint_result.status();
      return false;
    }
  } catch (const std::exception& e) {
    ABSL_LOG(ERROR) << "Exception during endpoint recreation: " << e.what();
    return false;
  }
}

void ConnectionRecoveryManager::MarkEndpointFailed(EndpointInfo* endpoint_info) {
  endpoint_info->state = EndpointState::FAILED;

  ABSL_LOG(ERROR) << "Endpoint marked as failed: " << endpoint_info->address
                  << " (errors: " << endpoint_info->error_count
                  << ", recovery attempts: " << endpoint_info->recovery_attempts << ")";

  // TODO: Notify application of endpoint failure
  // Could trigger fallback mechanisms or user notification
}

} // namespace tensorstore
```

#### Step 3.5: Create Retry Logic with Circuit Breaker
**New File:** `retry_manager.h`

```cpp
#pragma once
#include "error_classification.h"
#include "absl/status/status.h"
#include <functional>
#include <chrono>
#include <atomic>

namespace tensorstore {

template<typename ResultType>
class RetryManager {
public:
  using OperationFunction = std::function<absl::StatusOr<ResultType>()>;

  struct RetryConfig {
    int max_attempts = 3;
    std::chrono::milliseconds base_delay = std::chrono::milliseconds(100);
    std::chrono::milliseconds max_delay = std::chrono::seconds(10);
    double backoff_multiplier = 2.0;
    bool enable_jitter = true;
  };

  explicit RetryManager(const RetryConfig& config = {}) : config_(config) {}

  // Execute operation with retry logic
  absl::StatusOr<ResultType> ExecuteWithRetry(
      OperationFunction operation,
      const std::string& operation_name = "unknown");

private:
  RetryConfig config_;
  std::chrono::milliseconds CalculateDelay(int attempt_number);
};

// Circuit breaker for preventing cascade failures
class CircuitBreaker {
public:
  enum class State {
    CLOSED,     // Normal operation
    OPEN,       // Failing fast
    HALF_OPEN   // Testing if service recovered
  };

  struct CircuitConfig {
    int failure_threshold = 5;           // Failures before opening circuit
    int success_threshold = 2;           // Successes to close circuit
    std::chrono::seconds timeout = std::chrono::seconds(30);  // How long to stay open
  };

  explicit CircuitBreaker(const CircuitConfig& config = {}) : config_(config) {}

  // Check if operation should be allowed
  bool ShouldAllowOperation();

  // Record operation result
  void RecordSuccess();
  void RecordFailure();

  State GetState() const { return state_; }

private:
  CircuitConfig config_;
  std::atomic<State> state_{State::CLOSED};
  std::atomic<int> failure_count_{0};
  std::atomic<int> success_count_{0};
  std::atomic<std::chrono::steady_clock::time_point> last_failure_time_;

  void OpenCircuit();
  void CloseCircuit();
};

} // namespace tensorstore
```

#### Step 3.6: Update UCX Error Handler
**File:** `remote_dram_kvstore.cc` lines 490-492

**Replace Basic Error Handler:**
```cpp
/// Enhanced UCX error handler with recovery capabilities
void EnhancedUcxErrorHandler(void* arg, ucp_ep_h ep, ucs_status_t status) {
  // Get context information if available
  std::string context = arg ? static_cast<char*>(arg) : "unknown";

  // Classify and log the error with full detail
  auto error_info = UCXErrorClassifier::ClassifyError(status, context);

  ABSL_LOG(ERROR) << "UCX Enhanced Error Handler: " << error_info.description
                  << " | Severity: " << static_cast<int>(error_info.severity)
                  << " | Category: " << static_cast<int>(error_info.category)
                  << " | Retryable: " << (error_info.max_retry_attempts > 0)
                  << " | Requires Reconnection: " << error_info.requires_reconnection;

  // Trigger connection recovery if endpoint is registered
  auto& recovery_manager = ConnectionRecoveryManager::Instance();
  recovery_manager.HandleEndpointError(ep, status, context);

  // Update global error metrics
  UcxManager::Instance().RecordError(error_info);

  // Log additional diagnostic information
  ABSL_LOG(INFO) << "UCX Error Diagnostics:"
                 << " | Endpoint: " << ep
                 << " | Status Code: " << status
                 << " | UCX String: " << ucs_status_string(status)
                 << " | Context: " << context;
}
```

#### Step 3.7: Integrate Error Handling into Operations
**File:** `remote_dram_kvstore.cc` - Update WriteRemote and ReadRemote

**Enhanced WriteRemote with Retry:**
```cpp
Future<TimestampedStorageGeneration> WriteRemoteWithRetry(const kvstore::Key& key,
                                                          const absl::Cord& value) {
  // Create retry manager for this operation
  RetryManager<TimestampedStorageGeneration>::RetryConfig retry_config;
  retry_config.max_attempts = 3;
  retry_config.base_delay = std::chrono::milliseconds(100);

  RetryManager<TimestampedStorageGeneration> retry_manager(retry_config);

  // Define the operation to retry
  auto operation = [this, &key, &value]() -> absl::StatusOr<TimestampedStorageGeneration> {
    // Check if endpoint is healthy before attempting operation
    auto& recovery_manager = ConnectionRecoveryManager::Instance();
    if (!recovery_manager.IsEndpointHealthy(client_endpoint_)) {
      return absl::UnavailableError("Endpoint not healthy for write operation");
    }

    // Perform the original write operation
    auto future = WriteRemoteOriginal(key, value);

    // Convert future to StatusOr (this might need to be async in real implementation)
    auto result = future.result();
    if (!result.ok()) {
      return result.status();
    }
    return *result;
  };

  // Execute with retry logic
  auto result = retry_manager.ExecuteWithRetry(operation, "RemoteDramWrite");

  if (result.ok()) {
    return MakeReadyFuture<TimestampedStorageGeneration>(*result);
  } else {
    return result.status();
  }
}
```

### Testing Strategy

#### Step 3.8: Error Handling Tests
```cpp
// Test error classification
TEST(UCXErrorClassifierTest, ClassifiesErrorsCorrectly) {
  auto info = UCXErrorClassifier::ClassifyError(UCS_ERR_UNREACHABLE);
  EXPECT_EQ(info.severity, UCXErrorSeverity::TRANSIENT);
  EXPECT_EQ(info.category, UCXErrorCategory::CONNECTION);
  EXPECT_TRUE(info.requires_reconnection);
  EXPECT_GT(info.max_retry_attempts, 0);
}

// Test connection recovery
TEST(ConnectionRecoveryManagerTest, RecoversFromConnectionFailure) {
  auto& recovery_manager = ConnectionRecoveryManager::Instance();

  // Simulate connection failure and recovery
  ucp_ep_h mock_endpoint = reinterpret_cast<ucp_ep_h>(0x12345);
  recovery_manager.RegisterEndpoint(mock_endpoint, "127.0.0.1:12345");

  // Simulate error
  recovery_manager.HandleEndpointError(mock_endpoint, UCS_ERR_CONNECTION_RESET, "test");

  // Wait for recovery attempt
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Verify recovery was attempted
  const auto& metrics = recovery_manager.GetMetrics();
  EXPECT_GT(metrics.total_errors.load(), 0);
}

// Test retry logic
TEST(RetryManagerTest, RetriesTransientFailures) {
  RetryManager<int> retry_manager;

  int attempt_count = 0;
  auto operation = [&attempt_count]() -> absl::StatusOr<int> {
    attempt_count++;
    if (attempt_count < 3) {
      return absl::UnavailableError("Transient failure");
    }
    return 42;  // Success on third attempt
  };

  auto result = retry_manager.ExecuteWithRetry(operation, "test_operation");
  ASSERT_OK(result.status());
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(attempt_count, 3);
}

// Test circuit breaker
TEST(CircuitBreakerTest, OpensOnRepeatedFailures) {
  CircuitBreaker circuit_breaker;

  // Cause failures to open circuit
  for (int i = 0; i < 6; ++i) {
    circuit_breaker.RecordFailure();
  }

  EXPECT_EQ(circuit_breaker.GetState(), CircuitBreaker::State::OPEN);
  EXPECT_FALSE(circuit_breaker.ShouldAllowOperation());
}
```

#### Fault Injection Tests
```cpp
TEST(FaultToleranceTest, HandlesNetworkPartition) {
  // Test recovery from network partition scenarios
}

TEST(FaultToleranceTest, HandlesMemoryExhaustion) {
  // Test graceful degradation under resource pressure
}

TEST(FaultToleranceTest, HandlesUCXVersionMismatch) {
  // Test error handling with incompatible UCX versions
}
```

### Validation Criteria

#### Reliability Requirements
- [ ] 99.9% recovery success rate for transient failures
- [ ] Graceful degradation under resource pressure
- [ ] No data loss during error recovery
- [ ] Fast failure detection (< 1 second)

#### Performance Requirements
- [ ] Error handling overhead < 5% during normal operation
- [ ] Recovery time < 10 seconds for connection failures
- [ ] No performance regression from error handling logic

#### Observability Requirements
- [ ] Comprehensive error metrics and logging
- [ ] Clear error classification and recovery status
- [ ] Integration with monitoring systems
- [ ] Actionable error messages

### Success Metrics
- [ ] Robust error handling across all failure modes
- [ ] Automatic recovery from 90%+ of transient failures
- [ ] Production-ready reliability and observability
- [ ] Foundation ready for Phase 4 optimizations

### Estimated Timeline
- **Development:** 6 days
- **Testing & Validation:** 4 days
- **Integration & Documentation:** 2 days
- **Total:** 2.5 weeks

### Rollback Plan
1. **Configuration Toggle:** Enable/disable enhanced error handling
2. **Fallback Mode:** Revert to basic error logging if issues occur
3. **Gradual Rollout:** Deploy to subset of endpoints first

### Next Phase Prerequisites
Phase 3 must demonstrate:
- Reliable error recovery in production
- No increase in error rates
- Improved system resilience metrics
- Comprehensive error observability