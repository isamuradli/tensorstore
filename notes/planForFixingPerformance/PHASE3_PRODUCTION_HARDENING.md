# Phase 3: UCX Production Validation - Detailed Implementation Plan

## Overview
**Timeline**: Weeks 7-8  
**Prerequisites**: Phase 1 & 2 completion (UCX multi-worker + zero-copy implemented)  
**Goal**: Production readiness with UCX-specific monitoring and performance validation  
**Expected Outcome**: Stable high-performance UCX system achieving 1-5 GB/s target

## Week 7: UCX Monitoring & Diagnostics

### **Task 7.1: UCX Performance Metrics**
**Priority**: HIGH - UCX-specific monitoring  
**Location**: UCX monitoring subsystem

#### UCX-Specific Performance Metrics
```cpp
struct UcxPerformanceMetrics {
  // UCX Progress metrics
  std::atomic<uint64_t> total_progress_calls{0};
  std::atomic<uint64_t> progress_calls_per_second{0};
  std::atomic<double> progress_efficiency{0.0};  // useful progress / total calls
  
  // UCX Transport metrics
  std::atomic<uint64_t> ucx_sends_completed{0};
  std::atomic<uint64_t> ucx_sends_failed{0};
  std::atomic<uint64_t> ucx_bytes_sent{0};
  
  // UCX-specific metrics
  internal_metrics::Counter<int64_t>& ucx_send_errors;
  internal_metrics::Counter<int64_t>& ucx_recv_errors;
  internal_metrics::Counter<int64_t>& ucx_timeouts;
  internal_metrics::Histogram<double>& ucx_progress_cycles_per_second;
  
  // Reliability metrics
  internal_metrics::Counter<int64_t>& background_ack_successes;
  internal_metrics::Counter<int64_t>& background_ack_failures;
  internal_metrics::Counter<int64_t>& reliability_retries;
  internal_metrics::Histogram<double>& background_ack_latency_ms;
  
  // Flow control metrics
  internal_metrics::Histogram<int64_t>& outstanding_operations_per_worker;
  internal_metrics::Counter<int64_t>& flow_control_backpressure_events;
  internal_metrics::Histogram<double>& endpoint_utilization_ratio;
  
  // Memory metrics
  internal_metrics::Histogram<int64_t>& memory_pool_utilization;
  internal_metrics::Counter<int64_t>& zero_copy_buffer_allocations;
  internal_metrics::Counter<int64_t>& fallback_buffer_allocations;
};

auto remote_dram_metrics = []() -> RemoteDramMetrics {
  return {
    TENSORSTORE_KVSTORE_COMMON_METRICS(remote_dram),
    
    // Throughput
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, bytes_written, "Total bytes written"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, bytes_read, "Total bytes read"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, operations_completed, "Operations completed"),
    
    // Latency  
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, write_latency_us, "Write latency (μs)"),
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, read_latency_us, "Read latency (μs)"),
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, fire_and_forget_latency_us, "Fire-and-forget latency (μs)"),
    
    // UCX-specific
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, ucx_send_errors, "UCX send errors"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, ucx_recv_errors, "UCX receive errors"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, ucx_timeouts, "UCX timeout errors"),
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, ucx_progress_cycles_per_second, "UCX progress cycles/sec"),
    
    // Reliability
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, background_ack_successes, "Background ACK successes"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, background_ack_failures, "Background ACK failures"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, reliability_retries, "Reliability tracker retries"),
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, background_ack_latency_ms, "Background ACK latency (ms)"),
    
    // Flow control
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, outstanding_operations_per_worker, "Outstanding ops per worker"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, flow_control_backpressure_events, "Flow control backpressure events"),
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, endpoint_utilization_ratio, "Endpoint utilization ratio"),
    
    // Memory
    TENSORSTORE_KVSTORE_HISTOGRAM_IMPL(remote_dram, memory_pool_utilization, "Memory pool utilization %"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, zero_copy_buffer_allocations, "Zero-copy buffer allocations"),
    TENSORSTORE_KVSTORE_COUNTER_IMPL(remote_dram, fallback_buffer_allocations, "Fallback buffer allocations")
  };
}();
```

#### Metrics Collection Implementation
```cpp
class MetricsCollector {
  RemoteDramMetrics& metrics_;
  std::thread metrics_thread_;
  std::atomic<bool> active_{true};
  
public:
  explicit MetricsCollector(RemoteDramMetrics& metrics) : metrics_(metrics) {
    metrics_thread_ = std::thread([this]() { MetricsCollectionLoop(); });
  }
  
  void RecordWriteOperation(absl::Duration latency, size_t bytes) {
    metrics_.write_latency_us.Observe(absl::ToDoubleMicroseconds(latency));
    metrics_.bytes_written.Increment(bytes);
    metrics_.operations_completed.Increment();
  }
  
  void RecordFireAndForgetLatency(absl::Duration latency) {
    metrics_.fire_and_forget_latency_us.Observe(absl::ToDoubleMicroseconds(latency));
  }
  
  void RecordUcxError(ucs_status_t status) {
    switch (status) {
      case UCS_ERR_NO_MEMORY:
      case UCS_ERR_BUSY:
      case UCS_ERR_NO_RESOURCE:
        metrics_.ucx_send_errors.Increment();
        break;
      case UCS_ERR_TIMED_OUT:
        metrics_.ucx_timeouts.Increment();
        break;
      default:
        metrics_.ucx_send_errors.Increment();
    }
  }
  
  void RecordBackgroundAck(bool success, absl::Duration latency) {
    if (success) {
      metrics_.background_ack_successes.Increment();
    } else {
      metrics_.background_ack_failures.Increment();
    }
    metrics_.background_ack_latency_ms.Observe(absl::ToDoubleMilliseconds(latency));
  }
  
private:
  void MetricsCollectionLoop() {
    while (active_.load()) {
      CollectSystemMetrics();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  
  void CollectSystemMetrics() {
    // Collect UCX progress rate
    static size_t last_progress_count = 0;
    size_t current_progress_count = GetTotalProgressCycles();
    size_t progress_rate = current_progress_count - last_progress_count;
    metrics_.ucx_progress_cycles_per_second.Observe(progress_rate);
    last_progress_count = current_progress_count;
    
    // Collect memory pool utilization
    auto pool_stats = GetMemoryPoolStats();
    double utilization = (double(pool_stats.used_buffers) / pool_stats.total_buffers) * 100.0;
    metrics_.memory_pool_utilization.Observe(utilization);
    
    // Collect endpoint utilization
    auto endpoint_stats = GetEndpointUtilizationStats();
    for (double utilization : endpoint_stats.utilization_ratios) {
      metrics_.endpoint_utilization_ratio.Observe(utilization);
    }
  }
};
```

### **Task 7.2: Resource Monitoring**
**Priority**: HIGH - System health visibility  
**Location**: Resource usage tracking

#### UCX Resource Monitoring
```cpp
class UcxResourceMonitor {
  struct UcxResourceStats {
    size_t total_contexts = 0;
    size_t total_workers = 0;
    size_t total_endpoints = 0;
    size_t registered_memory_bytes = 0;
    size_t active_requests = 0;
  };
  
  std::atomic<UcxResourceStats> current_stats_;
  std::thread monitoring_thread_;
  
public:
  void StartMonitoring() {
    monitoring_thread_ = std::thread([this]() { MonitoringLoop(); });
  }
  
  UcxResourceStats GetCurrentStats() const {
    return current_stats_.load();
  }
  
private:
  void MonitoringLoop() {
    while (active_.load()) {
      CollectUcxResourceStats();
      CheckResourceLeaks();
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }
  
  void CollectUcxResourceStats() {
    UcxResourceStats stats{};
    
    // Collect from all worker contexts
    for (const auto& worker : GetAllWorkers()) {
      stats.total_workers++;
      stats.total_endpoints += worker->GetEndpointCount();
      stats.active_requests += worker->GetActiveRequestCount();
    }
    
    // Collect memory pool stats
    auto memory_stats = GetMemoryPoolStats();
    stats.registered_memory_bytes = memory_stats.total_registered_bytes;
    
    current_stats_.store(stats);
    
    // Update metrics
    remote_dram_metrics.Get().ucx_active_workers.Set(stats.total_workers);
    remote_dram_metrics.Get().ucx_active_endpoints.Set(stats.total_endpoints);
    remote_dram_metrics.Get().ucx_registered_memory_mb.Set(stats.registered_memory_bytes / (1024*1024));
  }
  
  void CheckResourceLeaks() {
    static UcxResourceStats last_stats{};
    UcxResourceStats current = current_stats_.load();
    
    // Detect potential memory leaks
    if (current.registered_memory_bytes > last_stats.registered_memory_bytes * 1.5) {
      LOG(WARNING) << "Potential UCX memory leak detected: " 
                   << current.registered_memory_bytes << " bytes registered";
    }
    
    // Detect endpoint leaks
    if (current.total_endpoints > last_stats.total_endpoints * 2) {
      LOG(WARNING) << "Potential UCX endpoint leak detected: " 
                   << current.total_endpoints << " endpoints active";
    }
    
    last_stats = current;
  }
};
```

### **Task 7.3: Alerting Integration**
**Priority**: HIGH - Critical error notifications  
**Location**: Alert system integration

#### Critical Alert Conditions
```cpp
class AlertingSystem {
  struct AlertThresholds {
    double max_error_rate = 0.05;        // 5% error rate triggers alert
    absl::Duration max_latency = absl::Milliseconds(100);  // >100ms latency alert
    double min_throughput_mbps = 100.0;  // <100 MB/s throughput alert
    size_t max_outstanding_ops = 1000;   // >1000 outstanding ops alert
    double max_memory_utilization = 0.9; // >90% memory pool utilization alert
  };
  
  AlertThresholds thresholds_;
  std::atomic<absl::Time> last_alert_time_{absl::InfinitePast()};
  static constexpr absl::Duration ALERT_COOLDOWN = absl::Minutes(5);
  
public:
  void CheckAlertConditions() {
    if (absl::Now() - last_alert_time_.load() < ALERT_COOLDOWN) {
      return;  // Rate limit alerts
    }
    
    CheckErrorRate();
    CheckLatency();
    CheckThroughput();
    CheckResourceUtilization();
  }
  
private:
  void CheckErrorRate() {
    auto stats = GetOperationStats();
    if (stats.total_operations > 100) {  // Minimum sample size
      double error_rate = double(stats.failed_operations) / stats.total_operations;
      if (error_rate > thresholds_.max_error_rate) {
        SendAlert(AlertLevel::CRITICAL, 
                 absl::StrCat("High error rate: ", error_rate * 100, "%"));
      }
    }
  }
  
  void CheckLatency() {
    auto latency_stats = GetLatencyStats();
    if (latency_stats.p99_latency > thresholds_.max_latency) {
      SendAlert(AlertLevel::WARNING,
               absl::StrCat("High P99 latency: ", 
                           absl::ToDoubleMilliseconds(latency_stats.p99_latency), "ms"));
    }
  }
  
  void CheckThroughput() {
    auto throughput_stats = GetThroughputStats();
    if (throughput_stats.current_mbps < thresholds_.min_throughput_mbps) {
      SendAlert(AlertLevel::WARNING,
               absl::StrCat("Low throughput: ", throughput_stats.current_mbps, " MB/s"));
    }
  }
  
  void SendAlert(AlertLevel level, const std::string& message) {
    LOG(ERROR) << "REMOTE_DRAM_ALERT [" << AlertLevelToString(level) << "]: " << message;
    
    // Integration with monitoring systems (Prometheus, etc.)
    if (auto* alert_handler = GetAlertHandler()) {
      alert_handler->SendAlert({
        .level = level,
        .component = "remote_dram_kvstore",
        .message = message,
        .timestamp = absl::Now()
      });
    }
    
    last_alert_time_.store(absl::Now());
  }
};
```

## Week 8: Testing & Validation

### **Task 8.1: Performance Benchmarking**
**Priority**: CRITICAL - Validate target achievement  
**Location**: Comprehensive performance test suite

#### Throughput Validation Tests
```cpp
class ThroughputBenchmark {
public:
  struct BenchmarkConfig {
    size_t num_operations = 10000;
    size_t value_size = 16 * 1024;  // 16KB values
    size_t num_concurrent_writers = 100;
    absl::Duration test_duration = absl::Seconds(60);
  };
  
  struct BenchmarkResults {
    double throughput_mbps;
    double throughput_ops_per_sec;
    absl::Duration avg_latency;
    absl::Duration p50_latency;
    absl::Duration p99_latency;
    absl::Duration p999_latency;
    double error_rate;
  };
  
  BenchmarkResults RunThroughputBenchmark(const BenchmarkConfig& config) {
    LOG(INFO) << "Starting throughput benchmark: " 
              << config.num_operations << " ops, " 
              << config.value_size << " byte values, "
              << config.num_concurrent_writers << " concurrent writers";
    
    auto start_time = absl::Now();
    std::vector<absl::Duration> latencies;
    std::atomic<size_t> completed_ops{0};
    std::atomic<size_t> failed_ops{0};
    
    // Launch concurrent writers
    std::vector<std::thread> writers;
    for (size_t i = 0; i < config.num_concurrent_writers; ++i) {
      writers.emplace_back([&, i]() {
        RunWriterThread(i, config, latencies, completed_ops, failed_ops);
      });
    }
    
    // Wait for completion
    for (auto& writer : writers) {
      writer.join();
    }
    
    auto end_time = absl::Now();
    auto total_duration = end_time - start_time;
    
    return CalculateResults(config, latencies, completed_ops.load(), 
                           failed_ops.load(), total_duration);
  }
  
private:
  void RunWriterThread(size_t writer_id, const BenchmarkConfig& config,
                      std::vector<absl::Duration>& latencies,
                      std::atomic<size_t>& completed_ops,
                      std::atomic<size_t>& failed_ops) {
    
    size_t ops_per_writer = config.num_operations / config.num_concurrent_writers;
    std::string value(config.value_size, 'x');
    
    for (size_t i = 0; i < ops_per_writer; ++i) {
      std::string key = absl::StrCat("benchmark_", writer_id, "_", i);
      
      auto start = absl::Now();
      auto future = kvstore::Write(store_, key, absl::Cord(value));
      auto result = future.result();
      auto latency = absl::Now() - start;
      
      if (result.ok()) {
        completed_ops.fetch_add(1);
        // Thread-safe latency recording
        {
          absl::MutexLock lock(&latency_mutex_);
          latencies.push_back(latency);
        }
      } else {
        failed_ops.fetch_add(1);
        LOG(ERROR) << "Write failed: " << result.status();
      }
    }
  }
  
  BenchmarkResults CalculateResults(const BenchmarkConfig& config,
                                   const std::vector<absl::Duration>& latencies,
                                   size_t completed_ops, size_t failed_ops,
                                   absl::Duration total_duration) {
    
    BenchmarkResults results{};
    
    // Calculate throughput
    size_t total_bytes = completed_ops * config.value_size;
    results.throughput_mbps = (double(total_bytes) / (1024 * 1024)) / 
                             absl::ToDoubleSeconds(total_duration);
    results.throughput_ops_per_sec = double(completed_ops) / 
                                    absl::ToDoubleSeconds(total_duration);
    
    // Calculate latency percentiles
    if (!latencies.empty()) {
      auto sorted_latencies = latencies;
      std::sort(sorted_latencies.begin(), sorted_latencies.end());
      
      results.avg_latency = std::accumulate(sorted_latencies.begin(), 
                                           sorted_latencies.end(), 
                                           absl::ZeroDuration()) / sorted_latencies.size();
      results.p50_latency = sorted_latencies[sorted_latencies.size() * 0.50];
      results.p99_latency = sorted_latencies[sorted_latencies.size() * 0.99];
      results.p999_latency = sorted_latencies[sorted_latencies.size() * 0.999];
    }
    
    // Calculate error rate
    size_t total_attempts = completed_ops + failed_ops;
    results.error_rate = total_attempts > 0 ? double(failed_ops) / total_attempts : 0.0;
    
    return results;
  }
};
```

#### Target Validation
```cpp
TEST(Phase3ValidationTest, ThroughputTargets) {
  ThroughputBenchmark benchmark;
  ThroughputBenchmark::BenchmarkConfig config{
    .num_operations = 50000,
    .value_size = 16 * 1024,
    .num_concurrent_writers = 200,
    .test_duration = absl::Seconds(30)
  };
  
  auto results = benchmark.RunThroughputBenchmark(config);
  
  LOG(INFO) << "Benchmark Results:";
  LOG(INFO) << "  Throughput: " << results.throughput_mbps << " MB/s";
  LOG(INFO) << "  Operations/sec: " << results.throughput_ops_per_sec;
  LOG(INFO) << "  Avg Latency: " << absl::ToDoubleMilliseconds(results.avg_latency) << "ms";
  LOG(INFO) << "  P99 Latency: " << absl::ToDoubleMilliseconds(results.p99_latency) << "ms";
  LOG(INFO) << "  Error Rate: " << results.error_rate * 100 << "%";
  
  // Validate minimum targets
  EXPECT_GE(results.throughput_mbps, 1000.0) << "Should achieve at least 1 GB/s";
  EXPECT_LT(results.p99_latency, absl::Milliseconds(10)) << "P99 latency should be <10ms";
  EXPECT_LT(results.error_rate, 0.01) << "Error rate should be <1%";
  
  // Stretch targets
  if (results.throughput_mbps >= 5000.0) {
    LOG(INFO) << "EXCELLENT: Achieved stretch target of 5+ GB/s";
  }
}
```

### **Task 8.2: Reliability Testing**
**Priority**: CRITICAL - Validate data durability  
**Location**: Error injection and recovery tests

#### Error Injection Testing
```cpp
class ReliabilityTester {
  enum class FailureType {
    NETWORK_PARTITION,
    SERVER_CRASH,
    UCX_TIMEOUT,
    MEMORY_EXHAUSTION,
    ENDPOINT_FAILURE
  };
  
public:
  void RunReliabilityTest(FailureType failure_type) {
    LOG(INFO) << "Starting reliability test: " << FailureTypeToString(failure_type);
    
    // Start background write operations
    auto write_task = std::async(std::launch::async, [this]() {
      return RunContinuousWrites();
    });
    
    // Wait for steady state
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    // Inject failure
    InjectFailure(failure_type);
    
    // Monitor recovery
    auto recovery_time = MonitorRecovery();
    
    // Stop writes and validate results
    StopWrites();
    auto results = write_task.get();
    
    ValidateReliabilityResults(results, recovery_time, failure_type);
  }
  
private:
  struct WriteResults {
    size_t total_writes = 0;
    size_t successful_writes = 0;
    size_t failed_writes = 0;
    size_t data_loss_count = 0;
    absl::Duration test_duration = absl::ZeroDuration();
  };
  
  WriteResults RunContinuousWrites() {
    WriteResults results{};
    auto start_time = absl::Now();
    
    while (continue_writes_.load()) {
      std::string key = absl::StrCat("reliability_test_", results.total_writes);
      std::string value(1024, 'R');  // Smaller values for reliability testing
      
      auto future = kvstore::Write(store_, key, absl::Cord(value));
      auto status = future.result();
      
      results.total_writes++;
      if (status.ok()) {
        results.successful_writes++;
        
        // Schedule background verification
        ScheduleDataVerification(key, value);
      } else {
        results.failed_writes++;
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    results.test_duration = absl::Now() - start_time;
    
    // Wait for background verifications to complete
    WaitForVerifications();
    results.data_loss_count = GetDataLossCount();
    
    return results;
  }
  
  void InjectFailure(FailureType failure_type) {
    switch (failure_type) {
      case FailureType::NETWORK_PARTITION:
        InjectNetworkPartition();
        break;
      case FailureType::SERVER_CRASH:
        InjectServerCrash();
        break;
      case FailureType::UCX_TIMEOUT:
        InjectUcxTimeouts();
        break;
      case FailureType::MEMORY_EXHAUSTION:
        InjectMemoryExhaustion();
        break;
      case FailureType::ENDPOINT_FAILURE:
        InjectEndpointFailure();
        break;
    }
  }
  
  absl::Duration MonitorRecovery() {
    auto start_time = absl::Now();
    
    // Monitor system health and wait for recovery
    while (absl::Now() - start_time < absl::Minutes(2)) {
      if (IsSystemHealthy()) {
        auto recovery_time = absl::Now() - start_time;
        LOG(INFO) << "System recovered in " << absl::ToDoubleSeconds(recovery_time) << "s";
        return recovery_time;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    LOG(ERROR) << "System failed to recover within timeout";
    return absl::Minutes(2);  // Timeout
  }
  
  void ValidateReliabilityResults(const WriteResults& results, 
                                 absl::Duration recovery_time,
                                 FailureType failure_type) {
    
    LOG(INFO) << "Reliability Test Results:";
    LOG(INFO) << "  Total writes: " << results.total_writes;
    LOG(INFO) << "  Successful: " << results.successful_writes;
    LOG(INFO) << "  Failed: " << results.failed_writes;
    LOG(INFO) << "  Data loss: " << results.data_loss_count;
    LOG(INFO) << "  Recovery time: " << absl::ToDoubleSeconds(recovery_time) << "s";
    
    // Validate reliability requirements
    EXPECT_EQ(results.data_loss_count, 0) << "No data loss should occur";
    EXPECT_LT(recovery_time, absl::Seconds(30)) << "Recovery should be <30s";
    
    double availability = double(results.successful_writes) / results.total_writes;
    EXPECT_GT(availability, 0.99) << "Availability should be >99%";
  }
};
```

### **Task 8.3: Load Testing**
**Priority**: HIGH - Scaling behavior validation  
**Location**: High-load stress tests

#### Scaling Load Test
```cpp
class LoadTester {
public:
  struct LoadTestConfig {
    size_t initial_load = 100;      // ops/sec
    size_t max_load = 100000;       // ops/sec  
    absl::Duration ramp_duration = absl::Minutes(10);
    absl::Duration steady_duration = absl::Minutes(5);
    size_t value_size = 4096;
  };
  
  void RunLoadTest(const LoadTestConfig& config) {
    LOG(INFO) << "Starting load test: " << config.initial_load 
              << " -> " << config.max_load << " ops/sec";
    
    std::atomic<size_t> current_target_load{config.initial_load};
    std::atomic<bool> test_active{true};
    
    // Metrics collection
    LoadTestMetrics metrics;
    auto metrics_thread = std::thread([&]() { 
      CollectLoadTestMetrics(metrics, test_active); 
    });
    
    // Load generation
    auto load_thread = std::thread([&]() {
      GenerateLoad(current_target_load, config, test_active);
    });
    
    // Ramp up load
    RampUpLoad(current_target_load, config);
    
    // Steady state testing
    LOG(INFO) << "Entering steady state at " << config.max_load << " ops/sec";
    std::this_thread::sleep_for(config.steady_duration);
    
    // Shutdown
    test_active.store(false);
    load_thread.join();
    metrics_thread.join();
    
    ValidateLoadTestResults(metrics, config);
  }
  
private:
  struct LoadTestMetrics {
    std::vector<double> throughput_samples;
    std::vector<double> latency_samples;
    std::vector<double> error_rate_samples;
    std::vector<size_t> memory_usage_samples;
    std::atomic<size_t> total_operations{0};
    std::atomic<size_t> failed_operations{0};
  };
  
  void GenerateLoad(std::atomic<size_t>& target_load, 
                   const LoadTestConfig& config,
                   std::atomic<bool>& active) {
    
    std::string test_value(config.value_size, 'L');
    size_t operation_counter = 0;
    
    while (active.load()) {
      size_t current_target = target_load.load();
      auto interval = absl::Nanoseconds(1e9 / current_target);  // ops/sec -> ns/op
      
      auto start = absl::Now();
      
      // Execute operation
      std::string key = absl::StrCat("load_test_", operation_counter++);
      auto future = kvstore::Write(store_, key, absl::Cord(test_value));
      
      // Don't wait for completion in fire-and-forget mode
      // Just track the future for later validation
      TrackFuture(std::move(future));
      
      // Rate limiting
      auto elapsed = absl::Now() - start;
      if (elapsed < interval) {
        absl::SleepFor(interval - elapsed);
      }
    }
  }
  
  void RampUpLoad(std::atomic<size_t>& current_load, const LoadTestConfig& config) {
    auto start_time = absl::Now();
    auto ramp_increment = (config.max_load - config.initial_load) / 
                         absl::ToDoubleSeconds(config.ramp_duration);
    
    while (absl::Now() - start_time < config.ramp_duration) {
      auto elapsed = absl::Now() - start_time;
      size_t target_load = config.initial_load + 
                          (ramp_increment * absl::ToDoubleSeconds(elapsed));
      current_load.store(std::min(target_load, config.max_load));
      
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    current_load.store(config.max_load);
  }
};
```

## Phase 3 Success Criteria

### **Production Readiness Checklist**
- [ ] **Comprehensive Monitoring**: All critical metrics collected and dashboards operational
- [ ] **Alert System**: Critical error conditions trigger appropriate alerts
- [ ] **Performance Validation**: Target 1-5 GB/s throughput consistently achieved
- [ ] **Reliability Validation**: Zero data loss under failure conditions
- [ ] **Load Testing**: System stable under maximum expected load
- [ ] **Resource Monitoring**: No memory leaks or resource exhaustion
- [ ] **Recovery Testing**: System recovers from failures within SLA

### **Performance Validation Targets**
```cpp
// Minimum production requirements
EXPECT_GE(throughput_mbps, 1000.0);      // ≥1 GB/s sustained throughput
EXPECT_LT(p99_latency, absl::Milliseconds(10));  // <10ms P99 latency
EXPECT_LT(error_rate, 0.01);             // <1% error rate
EXPECT_EQ(data_loss_events, 0);          // Zero data loss
EXPECT_LT(recovery_time, absl::Seconds(30));     // <30s failure recovery

// Stretch goals  
EXPECT_GE(throughput_mbps, 5000.0);      // ≥5 GB/s peak throughput
EXPECT_LT(p99_latency, absl::Milliseconds(1));   // <1ms P99 latency
EXPECT_LT(error_rate, 0.001);            // <0.1% error rate
```

### **Enterprise Feature Validation**
- [ ] **Metrics Integration**: All metrics available in monitoring dashboards
- [ ] **Alerting**: Critical alerts properly routed and actionable
- [ ] **Resource Limits**: System operates within defined resource bounds
- [ ] **Graceful Degradation**: System maintains partial functionality under load
- [ ] **Observability**: Sufficient logging and tracing for production debugging

### **Final Deployment Readiness**
```cpp
TEST(ProductionReadinessTest, EndToEndValidation) {
  // Validate complete system under realistic load
  LoadTester tester;
  auto results = tester.RunProductionSimulation({
    .duration = absl::Hours(1),
    .target_throughput_gbps = 2.0,
    .concurrent_clients = 500,
    .failure_injection = true
  });
  
  // Validate all production requirements met
  EXPECT_TRUE(results.meets_sla) << "System must meet production SLA";
  EXPECT_EQ(results.data_integrity_violations, 0) << "No data integrity issues";
  EXPECT_LT(results.max_recovery_time, absl::Seconds(30)) << "Fast failure recovery";
  
  LOG(INFO) << "✅ System ready for production deployment";
}
```

Phase 3 completes the transformation of the remote_dram kvstore from a prototype to a production-ready enterprise system capable of sustained multi-GB/s performance with full observability and reliability guarantees.