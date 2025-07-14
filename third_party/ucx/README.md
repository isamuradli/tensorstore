# UCX Integration for TensorStore Remote DRAM Driver

This directory contains the Bazel configuration for integrating the Unified Communication X (UCX) framework as a dependency for the TensorStore remote_dram KVStore driver.

## Overview

UCX is a high-performance communication framework designed for HPC and data center applications. It provides:

- **UCS (UCX Service Layer)**: Core utilities and services
- **UCT (UCX Transport Layer)**: Low-level transport abstractions  
- **UCP (UCX Protocol Layer)**: High-level protocols and APIs

## Files

- `workspace.bzl`: Defines the UCX dependency for Bazel
- `ucx.BUILD.bazel`: Build configuration for UCX when building from source
- `system.BUILD.bazel`: Build configuration for using system-installed UCX
- `config/ucx_config.h`: Basic configuration header for UCX compilation
- `test_ucx_integration.cc`: Simple test to verify UCX integration
- `BUILD.bazel`: Test build configuration

## Usage

### Building from Source (Default)

The dependency is configured to download and build UCX v1.17.0 from source:

```bash
# Test UCX integration
bazel test //third_party/ucx:test_ucx_integration
```

### Using System UCX

To use a pre-installed system UCX instead:

```bash
# Configure to use system UCX
bazel build --define=use_system_ucx=true //your/target
```

## Integration with Remote DRAM Driver

The remote_dram KVStore driver will use UCX for:

1. **Direct Memory Access**: Zero-copy data transfers between nodes
2. **RDMA Operations**: Remote Direct Memory Access for high-performance communication
3. **Connection Management**: Reliable communication channels between client and server nodes
4. **Memory Registration**: Registration of memory regions for remote access

## Dependencies

UCX may require the following system dependencies:

- `rdma-core` or `libibverbs`: For RDMA support
- `numactl`: For NUMA-aware memory management
- `libpthread`: For threading support

## Configuration Options

Build flags:
- `--@ucx//:debug`: Enable UCX debug output and logging
- `--define=use_system_ucx=true`: Use system-installed UCX instead of building from source

## Version

- **UCX Version**: 1.17.0
- **API Compatibility**: UCX API v1.17
- **License**: BSD-3-Clause

## References

- [UCX Homepage](https://openucx.org/)
- [UCX GitHub Repository](https://github.com/openucx/ucx)
- [UCX Documentation](https://openucx.readthedocs.io/)
- [UCX API Reference](https://openucx.org/documentation/) 