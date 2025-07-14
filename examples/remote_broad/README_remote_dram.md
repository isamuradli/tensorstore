# TensorStore Remote DRAM Example

This example demonstrates the **remote_dram** kvstore backend for TensorStore, which enables direct memory-to-memory transfer of tensor data between two nodes using the UCX (Unified Communication X) framework.

## Overview

The remote_dram backend allows you to:
- **Store tensor data remotely**: Send tensors from one node to another node's memory
- **High-performance transfer**: Use UCX for optimized memory-to-memory transfers
- **Transparent API**: Use standard TensorStore APIs without worrying about the underlying communication

## Architecture

The remote_dram backend operates in two modes:

1. **Server Mode**: A node that acts as a remote memory store, listening for incoming connections
2. **Client Mode**: A node that connects to a server to write/read tensor data

```
┌─────────────┐    UCX/Network    ┌─────────────┐
│   Client    │ ───────────────► │   Server    │
│   Node 1    │                  │   Node 2    │
│             │                  │   (Memory   │
│ (Tensors)   │                  │    Store)   │
└─────────────┘                  └─────────────┘
```

## Files

- **`remote_dram_example.cc`**: Main example demonstrating tensor transfer between nodes
- **`test_remote_dram.sh`**: Automated test script for easy testing
- **`README_remote_dram.md`**: This documentation file

## Usage

### Method 1: Using the Test Script (Recommended)

The easiest way to test the remote_dram backend:

```bash
# Make the script executable
chmod +x examples/test_remote_dram.sh

# Run the automated test
./examples/test_remote_dram.sh
```

This script will:
1. Build the example
2. Start a server in the background  
3. Run the client to transfer data
4. Show both server and client outputs
5. Clean up automatically

### Method 2: Manual Testing

For manual testing or running on separate machines:

**Terminal 1 (Server Node):**
```bash
# Start the server
bazel run //examples:remote_dram_example -- \
    --mode=server \
    --listen_addr=0.0.0.0:12345
```

**Terminal 2 (Client Node):**
```bash
# Run the client (wait for server to start first)
bazel run //examples:remote_dram_example -- \
    --mode=client \
    --server_addr=127.0.0.1:12345
```

### Cross-Node Testing

To test between different machines:

**On Server Machine:**
```bash
bazel run //examples:remote_dram_example -- \
    --mode=server \
    --listen_addr=0.0.0.0:12345
```

**On Client Machine:**
```bash
bazel run //examples:remote_dram_example -- \
    --mode=client \
    --server_addr=<SERVER_IP>:12345
```

Replace `<SERVER_IP>` with the actual IP address of the server machine.

## Example Features Demonstrated

### 1. **Server Mode**
- Creates a remote DRAM kvstore that listens for connections
- Accepts tensor data from clients
- Stores data in server's memory
- Uses UCX for high-performance networking

### 2. **Client Mode**  
- Connects to a remote DRAM server
- Creates sample 3D tensor data (4x4x3 float array)
- Transfers tensor data using TensorStore's native APIs
- Tests both high-level TensorStore operations and direct kvstore operations

### 3. **TensorStore Integration**
The example shows two ways to use the remote_dram backend:

**High-level TensorStore API:**
```cpp
// Create TensorStore spec with remote_dram backend
auto store_spec = tensorstore::Spec::FromJson({
  {"driver", "array"},
  {"kvstore", {
    {"driver", "remote_dram"},
    {"remote_addr", server_addr}
  }},
  {"dtype", "float32"},
  {"shape", {4, 4, 3}}
});

// Write tensor data
tensorstore::Write(tensor, store);
```

**Direct kvstore API:**
```cpp
// Create kvstore spec
auto kvstore_spec = tensorstore::Spec::FromJson({
  {"driver", "remote_dram"},
  {"remote_addr", server_addr}
});

// Write key-value pairs directly
tensorstore::kvstore::Write(kvstore, "my_key", my_data);
```

## Expected Output

When running successfully, you should see output like:

**Server:**
```
=== TensorStore Remote DRAM Example ===
Mode: server
Starting remote DRAM server on 0.0.0.0:12345
UCX Manager initialized successfully
UCX listener created successfully on 0.0.0.0:12345
Remote DRAM server started successfully!
Server is ready to receive tensor data...
```

**Client:**
```
=== TensorStore Remote DRAM Example ===
Mode: client
Starting remote DRAM client, connecting to 127.0.0.1:12345
Connected to remote DRAM server successfully!

Original Tensor (shape: 4x4x3):
[0, 0.1, 0.2] [1, 1.1, 1.2] [2, 2.1, 2.2] 
[10, 10.1, 10.2] [11, 11.1, 11.2] [12, 12.1, 12.2] 
[20, 20.1, 20.2] [21, 21.1, 21.2] [22, 22.1, 22.2] 

Creating TensorStore with remote DRAM backend...
TensorStore opened successfully!

Writing tensor data to remote DRAM...
✓ Tensor data written to remote DRAM successfully!

Testing direct kvstore operations...
✓ Key-value pair written successfully!
  Key: test_key_1
  Value: Hello from TensorStore remote DRAM!

Client operations completed successfully!
```

## Configuration Options

### Command Line Flags

- `--mode`: Set to "server" or "client"
- `--listen_addr`: Server listen address (server mode only)
- `--server_addr`: Server address to connect to (client mode only)

### JSON Spec Options

**Server Mode:**
```json
{
  "driver": "remote_dram",
  "listen_addr": "0.0.0.0:12345"
}
```

**Client Mode:**
```json
{
  "driver": "remote_dram", 
  "remote_addr": "127.0.0.1:12345"
}
```

## Technical Details

### Dependencies
- **UCX (Unified Communication X)**: High-performance communication framework
- **TensorStore**: Core tensor storage and manipulation library
- **Abseil**: C++ common libraries

### Communication Protocol
- Uses UCX for low-latency, high-bandwidth communication
- Custom message protocol for key-value operations
- Asynchronous operations with TensorStore's Future/Promise system

### Current Limitations
- Read operations are not yet fully implemented (as noted in the planning document)
- This example primarily demonstrates write operations
- Designed for demonstration and testing purposes

## Troubleshooting

### Common Issues

1. **"UCX Manager initialization failed"**
   - Ensure UCX library is properly installed and configured
   - Check network permissions and firewall settings

2. **"Failed to create UCX listener"**
   - Port might be already in use
   - Try a different port number
   - Check if you have permission to bind to the specified address

3. **"Failed to create UCX endpoint"**
   - Server might not be running
   - Check network connectivity between client and server
   - Verify server address and port are correct

4. **Build errors**
   - Ensure all dependencies are properly configured in the Bazel WORKSPACE
   - Check that UCX is available on your system

### Network Configuration

For cross-node testing, ensure:
- UCX is installed on both machines
- Network connectivity exists between nodes
- Firewall allows traffic on the chosen port
- No other services are using the same port

## Integration with TensorStore

This example demonstrates how the remote_dram backend integrates seamlessly with TensorStore's architecture:

1. **Driver Registration**: The remote_dram driver is registered with TensorStore's kvstore system
2. **Spec-based Configuration**: Uses JSON specs for configuration, consistent with other TensorStore drivers
3. **Asynchronous Operations**: Leverages TensorStore's Future/Promise framework for non-blocking operations
4. **Standard APIs**: Works with all standard TensorStore operations (create, open, read, write)

The remote_dram backend extends TensorStore's capabilities to distributed scenarios while maintaining the same user-friendly APIs. 