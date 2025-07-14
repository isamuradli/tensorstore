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

#include <iostream>

// Test that UCX headers can be included
#ifdef TENSORSTORE_SYSTEM_UCX
// When using system UCX, headers should be available via system paths
#include <ucp/api/ucp.h>
#include <uct/api/uct.h>
#include <ucs/type/status.h>
#else
// When building UCX from source, include from the ucx workspace
// Note: This is a placeholder - actual header paths will depend on
// the UCX source structure once it's downloaded and built
#endif

int main() {
    std::cout << "UCX integration test" << std::endl;
    
    // Test basic UCX version query
    #ifdef UCP_API_MAJOR
    std::cout << "UCX UCP API version: " << UCP_API_MAJOR << "." 
              << UCP_API_MINOR << std::endl;
    #endif
    
    // Test basic UCX status codes
    std::cout << "UCX_OK status: " << UCS_OK << std::endl;
    
    std::cout << "UCX integration test completed successfully!" << std::endl;
    return 0;
} 