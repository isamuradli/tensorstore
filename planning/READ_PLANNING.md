roject Goal

Rules:
REview and understand remote_dram kvstore.
NOTE: We do not change core tensorstore functionality or codebase. We will use its APIs to create examples and all changes to be done if necessary should be on custom kvstore backend "remote_dram". Rememmber this! 
To create a new TensorStore KVStore driver named remote_dram that enables direct memory-to-memory transfer of tensor data between two clients (nodes) using the UCX communication framework. The entire implementation will be an extension to the existing TensorStore codebase, leveraging its build (bazel) and testing systems, while preserving the high-level abstraction of the TensorStore API.
We are currently testing the code in single machine node locally. Only after it successfully tested and workes I will allocate two nodes and run the server and client in seperate nodes.
USE tensorstore native BAZEL to build. Every code modification has to be follow tensorstore native code practices.
ALL custom test file has to be inside examples folder
In all examples always use tensorstore apis. read custom_example.cc to see how it build jsonspec
READ/WRITE has to be done by tensorstore, data transfer has to be done by UCX communication layer
end of rules






*** Phase 1: Analysis and Foundation***
This phase focuses on understanding the existing TensorStore architecture for remote backends and setting up the foundational components for our new driver.


1.1. Analysis of Existing Remote KVStore Drivers (S3, GCS)
An analysis of tensorstore/kvstore/s3/ and tensorstore/kvstore/gcs/ reveals a common pattern for integrating remote, asynchronous storage systems:

Driver Registration: A new driver is made available to TensorStore by a registration mechanism. This is typically done in a registry.cc file using a macro like TENSORSTORE_KVSTORE_DRIVER_REGISTER. This links the driver's string identifier (e.g., "s3", "gcs") to its factory class.

Driver Class (...Driver): This class inherits from kvstore::Driver. Its primary role is to parse the JsonSpec provided by the user (e.g., bucket names, credentials, endpoints) and to open a KvStore instance. It acts as a factory and configuration manager.

KvStore Class (...KvStore): This class inherits from kvstore::KvStore. This is the core of the implementation. It must override virtual functions for data operations:

Read(Key, options)

Write(Key, value, options)

DeleteRange(Range, options)

Asynchronous Operations: TensorStore is heavily asynchronous, using a custom Future and Promise framework (tensorstore/util/future.h). Existing drivers wrap their underlying asynchronous libraries (e.g., HTTP clients) by:

Initiating a non-blocking I/O operation.

Passing a callback to the I/O library.

This callback, upon completion, fulfills a TensorStore Promise, which in turn resolves the Future returned to the initial caller.

Our remote_dram driver will replicate this pattern, substituting the HTTP client with a UCX communication manager.

1.2. Key Architectural Decision: Client/Server Model
The remote_dram driver will operate in one of two modes, determined by the JSON spec:

Server Mode: A TensorStore instance on one node (e.g., Node 2) will act as the "memory server." It listens for incoming UCX connections and requests. It will be configured with a listen_addr.

Client Mode: A TensorStore instance on another node (e.g., Node 1) acts as the client. It initiates connections to the server to write or read data. It will be configured with a remote_addr.

This allows for a symmetric setup where a single driver can perform both roles.

Phase 2: Development Environment Setup
This phase prepares the build environment by integrating the UCX dependency into Bazel.

2.1. Update Bazel WORKSPACE
The UCX library needs to be added as an external dependency for Bazel.

Action: Modify the WORKSPACE file in the root of the TensorStore project to include UCX. A new_local_repository or a similar rule can be used if UCX is pre-installed on the cluster nodes. For portability, a new_git_repository or http_archive pointing to the official UCX repository is preferable.
2.2. Create Bazel BUILD File for UCX
A BUILD file is needed to tell Bazel how to build UCX and what its public headers are.

Action: Create a third_party/ucx.BUILD file. This will define a cc_library target that other parts of the project can depend on.


Phase 3: Iterative Implementation Plan
This plan breaks down the development of the remote_dram driver into logical, testable steps.

Iteration 1: Scaffolding and Driver Registration
Goal: Create the basic file structure and register the remote_dram driver so TensorStore can recognize it.

Create Directory: Create tensorstore/kvstore/remote_dram/.

Create Files: Inside this directory, create:

remote_dram_kvstore.h

remote_dram_kvstore.cc

registry.cc

BUILD

Implement registry.cc:

Include tensorstore/kvstore/registry.h.

Use the TENSORSTORE_KVSTORE_DRIVER_REGISTER macro to register RemoteDramKvStoreDriver with the identifier "remote_dram".

Implement BUILD file:

Define a cc_library named remote_dram.

Add dependencies: //tensorstore/kvstore:kvstore, //tensorstore/util:json_bindable, and your new @ucx//:ucx target.

Define RemoteDramKvStoreDriver: In remote_dram_kvstore.h and .cc, create a minimal RemoteDramKvStoreDriver class inheriting from kvstore::Driver.

Implement the Open method to simply return an "unimplemented" error for now.

Define a Spec member and use tensorstore::internal::ApplyJsonBinding to parse listen_addr and remote_addr.

Test: After this iteration, tensorstore.open({"driver": "remote_dram"}) should fail with a recognizable error from your Open method, not a "driver not found" error.

Iteration 2: UCX Manager & Server Initialization
Goal: Abstract UCX initialization and implement the server-side listener.

UCX Manager: Create a singleton class, UcxManager, to handle the global UCX state (ucp_context_h). This avoids re-initialization.

UCX Worker: Within the manager, create a ucp_worker_h. This worker is the primary mechanism for polling for communication progress. It needs to be driven continuously.

Integrate with TensorStore's thread pool by submitting a task that repeatedly calls ucp_worker_progress.

Server Logic: In the RemoteDramKvStoreDriver::Open method:

If the spec contains a listen_addr, initiate the server.

This involves creating a UCX listener (ucp_listener_h) on the specified address and port.

The listener's callback will be responsible for creating new endpoints (ucp_ep_h) for incoming client connections.

Test: On one node, run a test that calls tensorstore.open({"driver": "remote_dram", "listen_addr": "0.0.0.0:12345"}). The test should hang (as it's now a long-running server), and logs should indicate that a UCX listener has been created successfully.

Iteration 3: Implement Client Write Operation
Goal: Implement the client-side Write method to send data to the server.

Client Connection: In RemoteDramKvStoreDriver::Open, if the spec contains a remote_addr, create a client-side RemoteDramKvStore object. This object will, on creation, establish a UCX endpoint (ucp_ep_h) to the server's listen_addr.

Implement Write:

The Write method will receive a Key and a Value (absl::Cord).

It will construct a simple message containing the key and value data.

It will use ucp_tag_send_nbx to send this message to the server. This is a non-blocking operation.

A Promise is created and its corresponding Future is returned immediately to the TensorStore framework.

Asynchronous Handling:

The ucp_tag_send_nbx call takes a callback function. This function will be invoked by the UCX progress engine upon completion of the send operation.

Inside this callback, fulfill the Promise created in the Write method.

Server-Side Write Handling:

The server-side needs a receive buffer. It should post a non-blocking receive (ucp_tag_recv_nb) to accept incoming messages.

When a message is received (the send from the client completes), the receive callback on the server is triggered.

This callback will parse the message, extract the key and value, and store the value in an in-memory std::map<std::string, absl::Cord> that acts as the server's DRAM storage.


Test: Write a 2-node test.

Node 2 (Server): Opens the driver in listen mode.

Node 1 (Client): Opens the driver in client mode, pointing to Node 2. It then creates a TensorStore array and commits it.

Verification: After the client write completes, inspect the server's in-memory map to verify the data was received correctly.


Iteration 4: Implement Client Read Operation
Goal: Implement the client-side Read method to retrieve data from the server.

Implement Read:

The Read method receives a Key.

It creates a Promise/Future pair.

It sends a "read request" message to the server containing the Key using ucp_tag_send_nbx.

Server-Side Read Handling:

The server's receive callback now needs to differentiate between a "write" message and a "read request."

Upon receiving a "read request," it looks up the key in its in-memory map.

It sends the corresponding value back to the client endpoint. If the key is not found, it sends back an error/empty response.

Client-Side Data Reception:

The client must have a pending ucp_tag_recv_nb posted to receive the server's response.

When the data is received, the UCX callback is fired. This callback will parse the value from the message and use it to fulfill the Promise that was created in the Read method.

Test: Extend the 2-node test. After the client writes data, it performs a tensorstore.read(). The test passes if the data read back matches the original data written



WhatShould Happen:

  1. Writer Client → UCX message → Server Process → Store in server's UcxManager
  2. Reader Client → UCX request → Server Process → Retrieve from server's UcxManager
  3. Server Process maintains the canonical storage that all clients interact with
  Comprehensive Fix Plan for Remote DRAM Client-Server Communication

  Problem Summary

  The current implementation incorrectly stores data in the client's local UcxManager singleton instead of sending it to the remote
  server process. This breaks the fundamental client-server model where the server should maintain canonical storage.

  Fix Strategy Overview

  1. Remove localhost shortcut logic that bypasses UCX communication
  2. Force real UCX networking even for localhost connections
  3. Ensure all data flows through the server process as the single source of truth
  4. Fix endpoint creation to establish real UCX connections
  5. Update notification system to work with proper UCX messaging

  Detailed Implementation Steps

  Step 1: Remove Localhost Detection Logic

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: UcxManager::CreateClientEndpoint() function (~line 300)

  Action: Remove or modify the localhost detection that creates dummy endpoints

  // REMOVE THIS SECTION:
  bool is_localhost = (server_addr.find("127.0.0.1") != std::string::npos ||
                      server_addr.find("localhost") != std::string::npos);

  if (is_localhost) {
    // ... dummy endpoint creation code
    ucp_ep_h dummy_endpoint = reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL);
    // ... rest of localhost logic
  }

  Replace with: Always use real UCX endpoint creation logic

  Step 2: Fix Write Operation Logic

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: RemoteDramDriver::WriteRemote() function (~line 1105)

  Action: Remove the localhost bypass logic

  // REMOVE THIS SECTION:
  if (client_endpoint_ == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
    ABSL_LOG(INFO) << "WriteRemote using localhost IPC for key '" << key << "' with " << value.size() << " bytes";

    // For localhost testing, directly store in the local server's storage
    auto& storage = UcxManager::Instance().GetStorage();
    storage.Store(key, value);

    // Notify the actual server process via TCP
    NotifyServerOfNewData(key, value);
    // ... return success
  }

  Replace with: Always use the real UCX messaging path (lines 1124+)

  Step 3: Fix Read Operation Logic

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: RemoteDramDriver::ReadRemote() function (~line 1240)

  Action: Remove the localhost bypass logic

  // REMOVE THIS SECTION:
  if (client_endpoint_ == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
    ABSL_LOG(INFO) << "ReadRemote using localhost IPC for key '" << key << "'";

    // For localhost testing, directly read from the local server's storage
    auto& storage = UcxManager::Instance().GetStorage();
    auto value = storage.Get(key);
    // ... return result
  }

  Replace with: Always use the real UCX messaging path (lines 1260+)

  Step 4: Update UCX Endpoint Creation

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: UcxManager::CreateClientEndpoint() function

  Action: Ensure all connections use real UCX networking

  Result<ucp_ep_h> UcxManager::CreateClientEndpoint(const std::string& server_addr) {
    absl::MutexLock lock(&mutex_);

    if (!initialized_) {
      return absl::FailedPreconditionError("UCX Manager not initialized");
    }

    ABSL_LOG(INFO) << "Creating UCX client endpoint to: " << server_addr;

    // Parse server address for UCX connection
    std::string host;
    uint16_t port;
    auto parse_result = ParseServerAddress(server_addr, &host, &port);
    if (!parse_result.ok()) {
      return parse_result;
    }

    // Create UCX endpoint parameters
    ucp_ep_params_t ep_params;
    struct sockaddr_in server_sockaddr;

    memset(&ep_params, 0, sizeof(ep_params));
    memset(&server_sockaddr, 0, sizeof(server_sockaddr));

    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_sockaddr.sin_addr);

    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.address = reinterpret_cast<const ucp_address_t*>(&server_sockaddr);
    ep_params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb = UcxErrorHandler;
    ep_params.err_handler.arg = nullptr;

    ucp_ep_h client_endpoint;
    ucs_status_t status = ucp_ep_create(worker_, &ep_params, &client_endpoint);

    if (status != UCS_OK) {
      return absl::InternalError(absl::StrFormat("Failed to create UCX endpoint: %s",
                                                ucs_status_string(status)));
    }

    RegisterClientSideEndpointNoLock(client_endpoint);

    ABSL_LOG(INFO) << "Successfully created UCX client endpoint to " << server_addr;
    return client_endpoint;
  }

  Step 5: Add Address Parsing Helper

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Action: Add a helper function to parse server addresses

  absl::Status ParseServerAddress(const std::string& server_addr, 
                                 std::string* host, 
                                 uint16_t* port) {
    size_t colon_pos = server_addr.find(':');
    if (colon_pos == std::string::npos) {
      return absl::InvalidArgumentError("Server address must be in format host:port");
    }

    *host = server_addr.substr(0, colon_pos);
    std::string port_str = server_addr.substr(colon_pos + 1);

    int port_int;
    if (!absl::SimpleAtoi(port_str, &port_int) || port_int <= 0 || port_int > 65535) {
      return absl::InvalidArgumentError("Invalid port number");
    }

    *port = static_cast<uint16_t>(port_int);
    return absl::OkStatus();
  }

  Step 6: Update UCX Cleanup Logic

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: UcxManager::Cleanup() function

  Action: Remove dummy endpoint special handling

  // REMOVE THIS SECTION:
  if (client_side_endpoint && client_side_endpoint != reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
    ABSL_LOG(INFO) << "Destroying client-side endpoint";
    ucp_ep_destroy(client_side_endpoint);
  } else if (client_side_endpoint == reinterpret_cast<ucp_ep_h>(0xDEADBEEFULL)) {
    ABSL_LOG(INFO) << "Skipping cleanup of localhost dummy endpoint";
  }

  Replace with: Standard endpoint cleanup

  if (client_side_endpoint) {
    ABSL_LOG(INFO) << "Destroying client-side endpoint";
    ucp_ep_destroy(client_side_endpoint);
  }

  Step 7: Remove TCP Notification System (Optional)

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Action: Since we're using proper UCX messaging, the TCP notification system is no longer needed

  - Remove NotifyServerOfNewData() function
  - Remove TCP notification logic from WriteRemote()
  - Remove TCP server thread from simple_remote_dram_server.cc (optional)

  Step 8: Update Server UCX Address Binding

  File: tensorstore/kvstore/remote_dram/remote_dram_kvstore.cc

  Location: UcxManager::StartServer() function

  Action: Ensure server binds to the correct address for UCX communication

  Verify that the server creates UCX listener on the same address/port that clients connect to.

  Step 9: Test the Fix

  File: Update test workflow

  1. Start server: bazel-bin/examples/simple_remote_dram_server
  2. Run writer: bazel-bin/examples/simple_writer_client
  3. Verify server logs: Should show "Server stored key-value pair" messages
  4. Check server status: Should show "1 keys stored" instead of "0 keys stored"
  5. Run reader: bazel-bin/examples/simple_reader_client
  6. Verify success: Reader should find and read the data successfully

  Step 10: Enhanced Logging for Debugging

  Action: Add comprehensive logging to track data flow

  // In WriteRemote():
  ABSL_LOG(INFO) << "WriteRemote: Sending UCX message for key='" << key
                 << "', value_size=" << value.size()
                 << ", endpoint=" << client_endpoint_;

  // In server receive callback:
  ABSL_LOG(INFO) << "Server: Received write request for key='" << key
                 << "', storing in server memory";

  // In ReadRemote():
  ABSL_LOG(INFO) << "ReadRemote: Sending UCX read request for key='" << key << "'";

  // In server read handling:
  ABSL_LOG(INFO) << "Server: Processing read request for key='" << key
                 << "', found=" << (value.has_value() ? "yes" : "no");

  Expected Results After Fix

  1. Writer Client: Sends data via UCX → Server Process stores data → Server shows "1 keys stored"
  2. Reader Client: Sends UCX read request → Server Process retrieves data → Returns data to reader
  3. All communication flows through the server as the canonical storage location
  4. Localhost and remote connections work identically through UCX messaging

  Files to Modify Summary

  1. remote_dram_kvstore.cc: Main implementation changes
  2. remote_dram_kvstore.h: Add ParseServerAddress declaration if needed
  3. Test workflow: Verify end-to-end functionality

  This fix will ensure that your goal of "1 client writes to server, 1 client reads from server" works correctly with proper
  inter-process communication through the server's memory