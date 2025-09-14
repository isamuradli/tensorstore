# Comprehensive Testing Strategy
## Remote DRAM KVStore Improvement Plan

This document outlines the comprehensive testing strategy to ensure each phase of the improvement plan is thoroughly validated before proceeding to the next phase.

## Testing Philosophy

### Risk-Driven Testing
Our testing approach prioritizes high-risk areas identified in the analysis:
1. **Memory management** (potential leaks, corruption)
2. **Concurrency issues** (race conditions, deadlocks)
3. **Performance regressions** (throughput, latency)
4. **Stability under load** (crashes, resource exhaustion)
5. **UCX integration** (compatibility, error handling)

### Quality Gates
Each phase must pass comprehensive testing before progression:
- **Unit Tests:** 100% pass rate, >90% coverage
- **Integration Tests:** All critical paths validated
- **Performance Tests:** Baseline maintained or improved
- **Stability Tests:** 24-72 hour continuous operation
- **Compatibility Tests:** Multiple UCX versions, system configurations

## Phase-Specific Testing Plans

### Phase 1: Worker Progress Thread Testing

#### Unit Tests
```cpp
// Test UCX native wait implementation
TEST(WorkerProgressTest, UCXNativeWaitFunctionality) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  // Verify ucp_worker_arm() and ucp_worker_wait() integration
  auto& metrics = manager.GetProgressMetrics();

  // Allow worker thread to run
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Verify native wait is being used (minimal fallback sleeps)
  EXPECT_LT(metrics.stats.fallback_sleeps.load(),
            metrics.stats.wait_cycles.load() * 0.1);  // <10% fallbacks

  manager.Shutdown();
}

TEST(WorkerProgressTest, ShutdownGracefully) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  auto start_time = std::chrono::steady_clock::now();
  manager.Shutdown();
  auto shutdown_time = std::chrono::steady_clock::now() - start_time;

  // Should shutdown quickly with native wait
  EXPECT_LT(shutdown_time, std::chrono::seconds(2));
}

TEST(WorkerProgressTest, HandleUCXWaitFailures) {
  // Test fallback behavior when ucp_worker_wait() fails
  // Mock UCX to return errors and verify graceful handling
}
```

#### Performance Tests
```cpp
TEST(Phase1PerformanceTest, CPUUsageReduction) {
  // Measure CPU usage before and after implementation
  auto baseline_cpu = MeasureCPUUsage(std::chrono::seconds(30));

  // Apply Phase 1 improvements
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  auto improved_cpu = MeasureCPUUsage(std::chrono::seconds(30));

  // Verify 60-80% reduction in idle CPU usage
  EXPECT_LT(improved_cpu, baseline_cpu * 0.4);

  manager.Shutdown();
}

TEST(Phase1PerformanceTest, LatencyImprovement) {
  // Benchmark operation latency before and after
  auto baseline_latency = BenchmarkOperationLatency(1000);  // 1000 operations

  // Apply improvements
  // ... initialization code ...

  auto improved_latency = BenchmarkOperationLatency(1000);

  // Verify 20-30% latency improvement
  EXPECT_LT(improved_latency.avg_latency_ms, baseline_latency.avg_latency_ms * 0.8);
}
```

#### Stability Tests
```cpp
TEST(Phase1StabilityTest, ContinuousOperation24Hours) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  // Run continuous operations for 24 hours
  auto test_duration = std::chrono::hours(24);
  auto start_time = std::chrono::steady_clock::now();

  std::atomic<bool> test_running{true};
  std::atomic<uint64_t> operation_count{0};
  std::atomic<uint64_t> error_count{0};

  // Worker thread performing operations
  std::thread worker([&]() {
    while (test_running.load()) {
      // Perform typical read/write operations
      auto result = PerformSampleOperation();
      operation_count++;
      if (!result.ok()) {
        error_count++;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // Monitor for test duration
  std::this_thread::sleep_for(test_duration);
  test_running = false;
  worker.join();

  // Verify stability metrics
  double error_rate = static_cast<double>(error_count) / operation_count;
  EXPECT_LT(error_rate, 0.01);  // Less than 1% error rate
  EXPECT_GT(operation_count, 100000);  // Reasonable operation count

  // Verify no memory leaks or resource exhaustion
  auto& metrics = manager.GetProgressMetrics();
  // Additional stability checks...

  manager.Shutdown();
}
```

### Phase 2: Memory Management Testing

#### Unit Tests
```cpp
TEST(UnifiedBufferManagerTest, AllocationAndDeallocation) {
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  auto& stats = UnifiedBufferManager::GetStatistics();
  auto initial_active = stats.active_buffers.load();

  // Test various buffer sizes
  std::vector<size_t> test_sizes = {1024, 64*1024, 1024*1024, 4*1024*1024};

  for (size_t size : test_sizes) {
    auto buffer_opt = UnifiedBufferManager::AllocateForSend(
        manager.GetContext(), size);
    ASSERT_TRUE(buffer_opt.has_value());

    auto& buffer = *buffer_opt;
    EXPECT_TRUE(buffer.is_valid());
    EXPECT_EQ(buffer.size(), size);
    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_NE(buffer.mem_handle(), nullptr);

    // Verify UCX registration
    EXPECT_TRUE(IsUCXRegistered(buffer.mem_handle()));
  }

  // Verify automatic cleanup
  EXPECT_EQ(stats.active_buffers.load(), initial_active);

  manager.Shutdown();
}

TEST(UnifiedBufferManagerTest, ZeroCopyOperations) {
  // Test that all operations use UCX registered memory
  // Verify zero-copy behavior through memory address tracking
}

TEST(UnifiedBufferManagerTest, MemoryLeakDetection) {
  // Comprehensive leak detection test
  auto& stats = UnifiedBufferManager::GetStatistics();
  auto initial_active = stats.active_buffers.load();

  {
    std::vector<UnifiedBufferManager::ManagedBuffer> buffers;

    // Allocate many buffers
    for (int i = 0; i < 1000; ++i) {
      auto buffer_opt = UnifiedBufferManager::AllocateForReceive(
          UcxManager::Instance().GetContext(), 1024 + i);
      if (buffer_opt) {
        buffers.push_back(std::move(*buffer_opt));
      }
    }

    EXPECT_EQ(stats.active_buffers.load(), initial_active + 1000);
  }

  // All buffers should be cleaned up
  EXPECT_EQ(stats.active_buffers.load(), initial_active);
}
```

#### Memory Safety Tests
```cpp
TEST(Phase2MemorySafetyTest, AddressSanitizerClean) {
  // Run with AddressSanitizer enabled
  // This test verifies no buffer overruns, use-after-free, etc.

  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  // Perform extensive operations that stress memory management
  for (int i = 0; i < 10000; ++i) {
    PerformRandomizedOperation();
  }

  manager.Shutdown();
  // AddressSanitizer will catch any issues
}

TEST(Phase2MemorySafetyTest, ValgrindClean) {
  // This test should be run under Valgrind to detect leaks
  // Script: valgrind --leak-check=full --error-exitcode=1 ./test

  // Perform comprehensive operation set
  ComprehensiveOperationSuite();

  // Any leaks will cause Valgrind to exit with error code 1
}
```

#### Performance Tests
```cpp
TEST(Phase2PerformanceTest, MemoryAllocationEfficiency) {
  // Compare allocation performance: unified vs mixed
  auto unified_perf = BenchmarkUnifiedAllocation(10000);

  // Verify 10-15% improvement
  EXPECT_GT(unified_perf.allocations_per_second, baseline_perf.allocations_per_second * 1.1);
  EXPECT_LT(unified_perf.avg_allocation_time, baseline_perf.avg_allocation_time * 0.9);
}

TEST(Phase2PerformanceTest, ZeroCopyEffectiveness) {
  // Measure actual zero-copy behavior
  auto zero_copy_stats = MeasureZeroCopyOperations(1000);

  // Verify true zero-copy (no intermediate buffers)
  EXPECT_EQ(zero_copy_stats.intermediate_copies, 0);
  EXPECT_GT(zero_copy_stats.direct_transfers, 990);  // >99% zero-copy
}
```

### Phase 3: Error Handling Testing

#### Unit Tests
```cpp
TEST(UCXErrorClassifierTest, ClassificationAccuracy) {
  // Test all UCX error codes for correct classification
  struct TestCase {
    ucs_status_t status;
    UCXErrorSeverity expected_severity;
    UCXErrorCategory expected_category;
    bool should_retry;
  };

  std::vector<TestCase> test_cases = {
    {UCS_ERR_UNREACHABLE, UCXErrorSeverity::TRANSIENT, UCXErrorCategory::CONNECTION, true},
    {UCS_ERR_NO_MEMORY, UCXErrorSeverity::RECOVERABLE, UCXErrorCategory::RESOURCE, true},
    {UCS_ERR_INVALID_PARAM, UCXErrorSeverity::PERMANENT, UCXErrorCategory::CONFIGURATION, false},
    // ... more test cases
  };

  for (const auto& test_case : test_cases) {
    auto error_info = UCXErrorClassifier::ClassifyError(test_case.status);
    EXPECT_EQ(error_info.severity, test_case.expected_severity);
    EXPECT_EQ(error_info.category, test_case.expected_category);
    EXPECT_EQ(UCXErrorClassifier::IsRetryable(test_case.status), test_case.should_retry);
  }
}

TEST(ConnectionRecoveryManagerTest, EndpointRecovery) {
  auto& recovery_manager = ConnectionRecoveryManager::Instance();

  // Mock endpoint for testing
  ucp_ep_h mock_endpoint = reinterpret_cast<ucp_ep_h>(0x12345);
  recovery_manager.RegisterEndpoint(mock_endpoint, "127.0.0.1:12345");

  // Simulate connection failure
  recovery_manager.HandleEndpointError(mock_endpoint, UCS_ERR_CONNECTION_RESET, "test");

  // Wait for recovery attempt
  std::this_thread::sleep_for(std::chrono::seconds(5));

  // Verify recovery was attempted
  const auto& metrics = recovery_manager.GetMetrics();
  EXPECT_GT(metrics.total_errors.load(), 0);
  EXPECT_GT(metrics.successful_recoveries.load() + metrics.failed_recoveries.load(), 0);
}

TEST(RetryManagerTest, ExponentialBackoff) {
  RetryManager<int> retry_manager;

  std::vector<std::chrono::steady_clock::time_point> attempt_times;
  int attempt_count = 0;

  auto operation = [&]() -> absl::StatusOr<int> {
    attempt_times.push_back(std::chrono::steady_clock::now());
    attempt_count++;
    if (attempt_count < 4) {
      return absl::UnavailableError("Transient failure");
    }
    return 42;  // Success on fourth attempt
  };

  auto result = retry_manager.ExecuteWithRetry(operation, "test");
  ASSERT_OK(result.status());
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(attempt_count, 4);

  // Verify exponential backoff timing
  EXPECT_GE(attempt_times.size(), 4);
  for (size_t i = 1; i < attempt_times.size(); ++i) {
    auto delay = attempt_times[i] - attempt_times[i-1];
    auto expected_min_delay = std::chrono::milliseconds(100 * (1 << (i-1)));
    EXPECT_GE(delay, expected_min_delay);
  }
}
```

#### Fault Injection Tests
```cpp
TEST(Phase3FaultInjectionTest, NetworkPartition) {
  // Test system behavior during network partition
  FaultInjector fault_injector;

  // Setup normal operation
  UcxManager& manager = UcxManager::Instance();
  ASSERT_OK(manager.Initialize());

  // Inject network partition
  fault_injector.InjectNetworkPartition(std::chrono::seconds(30));

  // Verify error handling and recovery
  auto& recovery_manager = ConnectionRecoveryManager::Instance();

  // Perform operations during partition
  for (int i = 0; i < 100; ++i) {
    auto result = PerformSampleOperation();
    // Operations should either succeed (cached) or fail gracefully
    if (!result.ok()) {
      EXPECT_TRUE(IsRetryableError(result.status()));
    }
  }

  // Remove partition
  fault_injector.RemoveNetworkPartition();

  // Verify recovery
  std::this_thread::sleep_for(std::chrono::seconds(10));
  const auto& metrics = recovery_manager.GetMetrics();
  EXPECT_GT(metrics.successful_recoveries.load(), 0);
}

TEST(Phase3FaultInjectionTest, MemoryExhaustion) {
  // Test graceful degradation under memory pressure
  MemoryPressureInjector pressure_injector;

  // Gradually increase memory pressure
  for (int pressure = 50; pressure <= 90; pressure += 10) {
    pressure_injector.SetMemoryPressure(pressure);  // Percentage

    auto operation_result = PerformBulkOperations(100);

    if (pressure < 80) {
      // Should handle moderate pressure gracefully
      EXPECT_GT(operation_result.success_rate, 0.9);
    } else {
      // Under extreme pressure, should degrade gracefully (not crash)
      EXPECT_GT(operation_result.success_rate, 0.1);  // Some operations succeed
      EXPECT_EQ(operation_result.crash_count, 0);     // No crashes
    }
  }

  pressure_injector.RemoveMemoryPressure();
}

TEST(Phase3FaultInjectionTest, UCXErrorStorm) {
  // Test handling of rapid UCX errors
  UCXErrorInjector error_injector;

  // Inject various UCX errors at high frequency
  std::vector<ucs_status_t> error_types = {
    UCS_ERR_CONNECTION_RESET,
    UCS_ERR_UNREACHABLE,
    UCS_ERR_BUSY,
    UCS_ERR_NO_MEMORY
  };

  error_injector.StartErrorStorm(error_types, 100);  // 100 errors/second

  // System should handle error storm without crashing
  auto storm_duration = std::chrono::seconds(30);
  auto start_time = std::chrono::steady_clock::now();

  while (std::chrono::steady_clock::now() - start_time < storm_duration) {
    auto result = PerformSampleOperation();
    // Operations may fail, but system should remain responsive
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  error_injector.StopErrorStorm();

  // Verify system can recover after error storm
  std::this_thread::sleep_for(std::chrono::seconds(10));

  auto recovery_result = PerformBulkOperations(50);
  EXPECT_GT(recovery_result.success_rate, 0.8);  // Should recover to high success rate
}
```

#### Circuit Breaker Tests
```cpp
TEST(CircuitBreakerTest, OpensAndCloses) {
  CircuitBreaker::CircuitConfig config;
  config.failure_threshold = 3;
  config.success_threshold = 2;
  config.timeout = std::chrono::seconds(5);

  CircuitBreaker circuit(config);

  // Initially closed
  EXPECT_EQ(circuit.GetState(), CircuitBreaker::State::CLOSED);
  EXPECT_TRUE(circuit.ShouldAllowOperation());

  // Cause failures to open circuit
  for (int i = 0; i < 4; ++i) {
    circuit.RecordFailure();
  }

  EXPECT_EQ(circuit.GetState(), CircuitBreaker::State::OPEN);
  EXPECT_FALSE(circuit.ShouldAllowOperation());

  // Wait for timeout
  std::this_thread::sleep_for(std::chrono::seconds(6));

  // Should be half-open
  EXPECT_EQ(circuit.GetState(), CircuitBreaker::State::HALF_OPEN);
  EXPECT_TRUE(circuit.ShouldAllowOperation());

  // Record successes to close circuit
  circuit.RecordSuccess();
  circuit.RecordSuccess();

  EXPECT_EQ(circuit.GetState(), CircuitBreaker::State::CLOSED);
}
```

### Phase 4: Performance and Scaling Testing

#### Root Cause Investigation Tests
```cpp
TEST(Phase4InvestigationTest, IdentifyStabilityCauses) {
  // Systematic investigation of why concurrency had to be reduced
  RootCauseInvestigation investigation;

  auto report = investigation.InvestigateStabilityIssues();

  // Verify investigation completeness
  EXPECT_FALSE(report.immediate_fixes.empty());
  EXPECT_FALSE(report.architectural_improvements.empty());

  // Apply identified fixes and re-test
  for (const auto& fix : report.immediate_fixes) {
    ApplyFix(fix);
  }

  // Verify fixes resolve issues
  auto post_fix_stability = TestSystemStability();
  EXPECT_TRUE(post_fix_stability.can_handle_higher_concurrency);
}
```

#### Scaling Tests
```cpp
TEST(Phase4ScalingTest, ProgressiveConcurrencyIncrease) {
  ConcurrencyStressTest stress_test;

  auto optimal_settings = stress_test.FindOptimalConcurrencySettings();

  // Verify we can exceed original conservative settings
  EXPECT_GE(optimal_settings.optimal_worker_threads, 4);
  EXPECT_GE(optimal_settings.optimal_batch_size, 8);
  EXPECT_GE(optimal_settings.optimal_server_buffers, 10);

  // Verify performance improvement
  EXPECT_GT(optimal_settings.achieved_throughput, baseline_throughput * 1.5);
  EXPECT_LT(optimal_settings.error_rate, 0.001);
}

TEST(Phase4ScalingTest, AdaptiveScaling) {
  AdaptiveScalingManager scaling_manager;
  scaling_manager.StartAdaptiveScaling();

  // Test scaling under different load patterns

  // High load should trigger scale up
  GenerateHighLoad(std::chrono::minutes(5));
  std::this_thread::sleep_for(std::chrono::seconds(60));
  EXPECT_GT(scaling_manager.GetCurrentWorkerThreads(), 2);

  // Low load should trigger scale down
  GenerateLowLoad(std::chrono::minutes(5));
  std::this_thread::sleep_for(std::chrono::seconds(60));

  // Error spike should trigger scale down
  InjectErrorSpike(std::chrono::minutes(2));
  std::this_thread::sleep_for(std::chrono::seconds(30));
  // Should reduce resources to improve stability

  scaling_manager.StopAdaptiveScaling();
}
```

#### Performance Benchmarking
```cpp
TEST(Phase4BenchmarkTest, ThroughputImprovement) {
  PerformanceBenchmark benchmark;

  // Measure baseline (Phase 3 performance)
  auto baseline = benchmark.RunThroughputBenchmark();

  // Apply Phase 4 optimizations
  ApplyPhase4Optimizations();

  // Measure optimized performance
  auto optimized = benchmark.RunThroughputBenchmark();

  // Verify 50-100% improvement
  EXPECT_GT(optimized.ops_per_second, baseline.ops_per_second * 1.5);
  EXPECT_LT(optimized.avg_latency_ms, baseline.avg_latency_ms * 0.7);

  // Verify reliability maintained
  EXPECT_LT(optimized.error_rate, 0.001);
}

TEST(Phase4BenchmarkTest, ScalabilityCharacteristics) {
  PerformanceBenchmark benchmark;

  std::vector<int> client_counts = {1, 2, 4, 8, 16, 32};
  std::vector<BenchmarkResults> results;

  for (int clients : client_counts) {
    auto config = benchmark.GetConfig();
    config.num_clients = clients;
    benchmark.SetConfig(config);

    results.push_back(benchmark.RunThroughputBenchmark());
  }

  // Verify linear or near-linear scaling up to optimal point
  for (size_t i = 1; i < results.size() && i < 4; ++i) {  // Test up to 8 clients
    double scaling_ratio = results[i].ops_per_second / results[0].ops_per_second;
    double expected_ratio = client_counts[i];

    // Should achieve at least 80% of linear scaling
    EXPECT_GT(scaling_ratio, expected_ratio * 0.8);
  }
}

TEST(Phase4BenchmarkTest, ExtendedStabilityTest) {
  // 72-hour stability test under optimized settings
  PerformanceBenchmark benchmark;
  auto config = benchmark.GetConfig();
  config.test_duration = std::chrono::hours(72);
  config.operations_per_client = 1000000;  // High operation count

  auto results = benchmark.RunStabilityBenchmark();

  // Verify sustained high performance
  EXPECT_GT(results.ops_per_second, 10000);  // High sustained throughput
  EXPECT_LT(results.error_rate, 0.0001);     // Very low error rate
  EXPECT_LT(results.max_latency_ms, 1000);   // Bounded tail latency

  // Verify resource stability
  EXPECT_LT(results.avg_memory_usage_mb, peak_memory_limit);
  EXPECT_LT(results.avg_cpu_usage, 0.8);     // CPU not saturated
}
```

## Testing Infrastructure

### Continuous Integration Pipeline

```yaml
# .github/workflows/comprehensive-testing.yml
name: Comprehensive Testing Pipeline

on: [push, pull_request]

jobs:
  phase1-validation:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Setup UCX Environment
        run: scripts/setup-ucx.sh
      - name: Build with UCX Native Wait
        run: bazel build //tensorstore/kvstore/remote_dram:all
      - name: Run Phase 1 Unit Tests
        run: bazel test //tensorstore/kvstore/remote_dram:phase1_tests
      - name: Run Performance Tests
        run: scripts/run-performance-tests.sh phase1
      - name: Run 24h Stability Test
        if: github.ref == 'refs/heads/main'
        run: scripts/run-stability-test.sh 24h

  phase2-validation:
    needs: phase1-validation
    runs-on: ubuntu-latest
    steps:
      - name: Run Memory Safety Tests
        run: |
          # AddressSanitizer
          bazel test --config=asan //tensorstore/kvstore/remote_dram:phase2_tests
          # MemorySanitizer
          bazel test --config=msan //tensorstore/kvstore/remote_dram:phase2_tests
      - name: Run Valgrind Tests
        run: scripts/run-valgrind-tests.sh
      - name: Run Memory Leak Detection
        run: scripts/run-leak-detection.sh

  phase3-validation:
    needs: phase2-validation
    runs-on: ubuntu-latest
    steps:
      - name: Run Fault Injection Tests
        run: bazel test //tensorstore/kvstore/remote_dram:fault_injection_tests
      - name: Run Error Recovery Tests
        run: scripts/run-error-recovery-tests.sh
      - name: Run Network Partition Tests
        run: scripts/run-network-partition-tests.sh

  phase4-validation:
    needs: phase3-validation
    runs-on: ubuntu-latest
    steps:
      - name: Run Performance Benchmarks
        run: scripts/run-benchmarks.sh
      - name: Run Scaling Tests
        run: scripts/run-scaling-tests.sh
      - name: Run Extended Stability Tests
        run: scripts/run-extended-stability.sh
```

### Test Environment Setup

#### UCX Test Environment
```bash
#!/bin/bash
# scripts/setup-ucx.sh

# Install UCX with proper configuration
sudo apt-get update
sudo apt-get install -y libnuma-dev librdmacm-dev libibverbs-dev

# Build UCX from source for latest features
git clone https://github.com/openucx/ucx.git
cd ucx
./autogen.sh
./configure --prefix=/usr/local --enable-debug --enable-profiling
make -j$(nproc)
sudo make install

# Configure environment
export UCX_TLS=tcp,self
export UCX_NET_DEVICES=lo
export UCX_LOG_LEVEL=info
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

#### Performance Testing Environment
```bash
#!/bin/bash
# scripts/setup-performance-environment.sh

# Configure system for performance testing
echo 'kernel.sched_rt_runtime_us = 1000000' | sudo tee -a /etc/sysctl.conf
echo 'kernel.sched_rt_period_us = 1000000' | sudo tee -a /etc/sysctl.conf
sudo sysctl -p

# Disable CPU frequency scaling
sudo cpupower frequency-set --governor performance

# Set up monitoring
sudo apt-get install -y htop iotop nethogs

# Create performance test workspace
mkdir -p /tmp/tensorstore-perf-tests
```

### Test Data Management

#### Test Data Generation
```cpp
class TestDataGenerator {
public:
  // Generate test data with various characteristics
  struct DataProfile {
    std::vector<size_t> key_sizes = {16, 64, 256, 1024};
    std::vector<size_t> value_sizes = {1024, 64*1024, 1024*1024, 4*1024*1024};
    std::vector<std::string> key_patterns = {"sequential", "random", "zipf"};
    int total_keys = 10000;
  };

  static std::vector<TestData> GenerateTestDataSet(const DataProfile& profile);
  static std::vector<TestData> GenerateRealisticWorkload();
  static std::vector<TestData> GenerateStressTestData();
};
```

#### Test Result Collection
```cpp
class TestResultCollector {
public:
  struct TestMetrics {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    uint64_t operations_completed;
    uint64_t operations_failed;
    std::vector<double> latency_samples;
    double cpu_usage_percent;
    size_t memory_usage_bytes;
    uint64_t network_bytes_sent;
    uint64_t network_bytes_received;
  };

  void StartCollection();
  void StopCollection();
  TestMetrics GetResults() const;

  void ExportResults(const std::string& filename, const std::string& format = "json");

private:
  void CollectSystemMetrics();
  std::thread metrics_thread_;
  std::atomic<bool> collecting_{false};
  TestMetrics current_metrics_;
};
```

### Test Reporting and Analysis

#### Automated Report Generation
```python
#!/usr/bin/env python3
# scripts/generate-test-report.py

import json
import matplotlib.pyplot as plt
import pandas as pd
from jinja2 import Template

class TestReportGenerator:
    def __init__(self, test_results_dir):
        self.results_dir = test_results_dir

    def generate_phase_report(self, phase_name):
        """Generate comprehensive report for a phase"""
        results = self.load_test_results(phase_name)

        # Generate performance charts
        self.create_performance_charts(results, phase_name)

        # Generate stability analysis
        self.create_stability_analysis(results, phase_name)

        # Generate HTML report
        self.create_html_report(results, phase_name)

    def create_performance_charts(self, results, phase_name):
        # Throughput over time
        plt.figure(figsize=(12, 6))
        plt.plot(results['timestamps'], results['throughput'])
        plt.title(f'{phase_name} - Throughput Over Time')
        plt.ylabel('Operations/Second')
        plt.xlabel('Time')
        plt.savefig(f'reports/{phase_name}_throughput.png')

        # Latency distribution
        plt.figure(figsize=(10, 6))
        plt.hist(results['latencies'], bins=50)
        plt.title(f'{phase_name} - Latency Distribution')
        plt.ylabel('Frequency')
        plt.xlabel('Latency (ms)')
        plt.savefig(f'reports/{phase_name}_latency.png')

if __name__ == '__main__':
    generator = TestReportGenerator('test_results/')
    for phase in ['phase1', 'phase2', 'phase3', 'phase4']:
        generator.generate_phase_report(phase)
```

## Quality Assurance Process

### Code Review Checklist
- [ ] All new code has corresponding unit tests
- [ ] Performance tests show no regressions
- [ ] Memory safety verified (AddressSanitizer, Valgrind)
- [ ] Thread safety verified (ThreadSanitizer)
- [ ] Error handling paths tested
- [ ] Documentation updated
- [ ] Compatibility tested across UCX versions

### Integration Testing Protocol
1. **Smoke Tests:** Basic functionality verification
2. **Regression Tests:** Ensure no existing functionality broken
3. **Performance Tests:** Baseline performance maintained or improved
4. **Stability Tests:** Extended operation without failures
5. **Compatibility Tests:** Multiple environments and configurations

### Deployment Readiness Criteria
Each phase must meet these criteria before deployment:
- [ ] 100% unit test pass rate
- [ ] Zero critical or high-severity bugs
- [ ] Performance targets met or exceeded
- [ ] 24+ hours continuous operation without failure
- [ ] Memory leak detection passes
- [ ] Security review completed (if applicable)
- [ ] Documentation and runbooks updated

This comprehensive testing strategy ensures that each phase of the improvement plan is thoroughly validated, reducing the risk of regressions and ensuring the overall success of the Remote DRAM KVStore transformation.