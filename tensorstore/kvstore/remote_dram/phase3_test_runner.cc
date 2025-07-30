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

/// Phase 3: Test Runner for Comprehensive Regression Testing

#include <iostream>
#include <vector>

#include "phase3_regression_test.h"
#include "absl/log/absl_log.h"

int main(int argc, char* argv[]) {
  
  std::cout << "=== Phase 3 Comprehensive Regression Test Suite ===\n" << std::endl;
  
  tensorstore::RegressionTestSuite test_suite;
  tensorstore::ContinuousPerformanceMonitor monitor;
  
  // Run all regression tests
  auto results = test_suite.RunAllTests();
  
  // Validate results
  bool all_passed = test_suite.ValidateAllTestsPassed(results);
  
  // Generate report
  std::string report = test_suite.GenerateTestReport(results);
  std::cout << report << std::endl;
  
  if (all_passed) {
    std::cout << "🎉 ALL TESTS PASSED - Phase 3 Validation Successful!" << std::endl;
    std::cout << "✅ 16KB Performance Preserved" << std::endl;
    std::cout << "✅ All Features Working Correctly" << std::endl;
    std::cout << "✅ No Regressions Detected" << std::endl;
    std::cout << "✅ Backward Compatibility Maintained" << std::endl;
    
    return 0;
  } else {
    std::cout << "❌ SOME TESTS FAILED - Phase 3 Validation Issues Detected!" << std::endl;
    std::cout << "Please review failed tests and fix issues before proceeding." << std::endl;
    
    return 1;
  }
}