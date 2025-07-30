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

#ifndef TENSORSTORE_KVSTORE_REMOTE_DRAM_PHASE3_REGRESSION_TEST_H_
#define TENSORSTORE_KVSTORE_REMOTE_DRAM_PHASE3_REGRESSION_TEST_H_

/// Phase 3: Comprehensive Regression Test Suite
/// This validates all phases don't break existing functionality

#include <memory>
#include <chrono>
#include <vector>
#include <string>

#include "tensorstore/kvstore/remote_dram/remote_dram_kvstore.h"
#include "absl/status/status.h"
#include "absl/log/absl_log.h"

namespace tensorstore {

/// Phase 3: Regression test suite for comprehensive validation
class RegressionTestSuite {
public:
  struct TestResult {
    bool passed;
    std::string test_name;
    std::string error_message;
    std::chrono::milliseconds duration;
  };
  
  struct PerformanceMetrics {
    double write_throughput_mbps;
    double read_throughput_mbps;
    std::chrono::milliseconds avg_latency;
    size_t total_operations;
    bool meets_16kb_baseline;
  };
  
  /// Test current 16KB performance exactly (critical test)
  TestResult TestCurrentPerformance();
  
  /// Test PerformanceBaseline class functionality
  TestResult TestPerformanceBaseline();
  
  /// Test LegacyBufferPool functionality
  TestResult TestLegacyBufferPool();
  
  /// Test enhanced buffer pool in LEGACY mode
  TestResult TestEnhancedBufferPoolLegacyMode();
  
  /// Test enhanced buffer pool in ENHANCED mode
  TestResult TestEnhancedBufferPoolEnhancedMode();
  
  /// Test memory monitoring functionality
  TestResult TestMemoryMonitoring();
  
  /// Test protocol handler functionality
  TestResult TestProtocolHandler();
  
  /// Test storage cleanup with safe defaults
  TestResult TestStorageCleanup();
  
  /// Test feature flags functionality
  TestResult TestFeatureFlags();
  
  /// Integration testing with feature combinations
  TestResult TestFeatureCombinations();
  
  /// Stress testing with high load
  TestResult TestHighLoad();
  
  /// Test memory pressure scenarios
  TestResult TestMemoryPressure();
  
  /// Run all regression tests
  std::vector<TestResult> RunAllTests();
  
  /// Validate all tests passed
  bool ValidateAllTestsPassed(const std::vector<TestResult>& results);
  
  /// Generate detailed test report
  std::string GenerateTestReport(const std::vector<TestResult>& results);
  
private:
  /// Helper to measure performance
  PerformanceMetrics MeasurePerformance(size_t data_size, size_t num_operations);
  
  /// Helper to create test data
  std::vector<char> CreateTestData(size_t size);
  
  /// Helper to validate performance meets baseline
  bool ValidatePerformanceBaseline(const PerformanceMetrics& metrics);
};

/// Phase 3: Continuous performance monitor for regression detection
class ContinuousPerformanceMonitor {
public:
  struct BaselineMetrics {
    double write_throughput_mbps;
    double read_throughput_mbps;
    std::chrono::milliseconds avg_latency;
    std::chrono::steady_clock::time_point recorded_time;
  };
  
  /// Start baseline measurement
  void StartBaseline();
  
  /// Record current baseline
  void RecordBaseline(const RegressionTestSuite::PerformanceMetrics& metrics);
  
  /// Detect regression compared to baseline
  bool DetectRegression(const RegressionTestSuite::PerformanceMetrics& current,
                       double threshold_percent = 5.0);
  
  /// Alert if regression detected
  void AlertOnRegression(const std::string& details);
  
  /// Get current baseline
  BaselineMetrics GetBaseline() const { return baseline_; }
  
private:
  BaselineMetrics baseline_;
  bool baseline_set_ = false;
};

}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REMOTE_DRAM_PHASE3_REGRESSION_TEST_H_