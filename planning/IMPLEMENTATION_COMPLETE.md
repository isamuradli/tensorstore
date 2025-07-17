

## Overview
This document analyzes the current remote_dram implementation's capability for server nodes to act as clients to communicate with other server nodes, and outlines implementation approaches for distributed server-to-server scenarios.

## Current Architecture Analysis

### ❌ **Current Limitation: Mutually Exclusive Design**

The existing implementation **explicitly prevents** server nodes from acting as clients due to architectural constraints:

```cpp
// From remote_dram_kvstore.cc line ~1252
if (data_.listen_addr.has_value() && data_.remote_addr.has_value()) {
  return absl::InvalidArgumentError(
      "Cannot specify both listen_addr and remote_addr");
}
```

### **Key Architectural Constraints**

#### 1. **Specification-Level Restriction**
- **Location**: `RemoteDramDriverSpec::DoOpen()`
- **Issue**: JSON spec validation rejects configurations with both `listen_addr` and `remote_addr`
- **Impact**: Cannot create a driver instance that operates in dual mode

#### 2. **Driver State Limitations**
```cpp
class RemoteDramDriver {
  bool is_server_mode_;           // Single boolean flag
  ucp_ep_h client_endpoint_;      // Single outbound connection
  // No support for multiple server connections
};
```

#### 3. **Read/Write Logic Branching**
```cpp
// Current exclusive branching logic
if (is_server_mode_) {
  return ReadLocal(key, options);
} else {
  return ReadRemote(key, options);
}
```

#### 4. **UcxManager Singleton Capabilities**
**✅ Positive Finding**: The underlying `UcxManager` singleton **does support both capabilities**:
- **Server Methods**: `CreateListener()`, `RegisterClientEndpoint()`, `PostServerReceive()`
- **Client Methods**: `CreateClientEndpoint()`, `RegisterPendingOperation()`
- **Shared Resources**: Single UCX context, worker, and storage instance

**Conclusion**: The limitation is at the driver specification/logic level, not the UCX transport level.

## Current Capabilities Matrix

| **Capability** | **Supported** | **Limitation** |
|----------------|---------------|----------------|
| **Single node listen for connections** | ✅ Yes | None |
| **Single node make outbound connections** | ✅ Yes | None |
| **Dual capability in same process** | ❌ No | Spec validation prevents it |
| **Multiple outbound connections** | ❌ No | Driver stores single endpoint |
| **Server-to-server communication** | ❌ No | No peer discovery/routing |
| **Key routing/partitioning** | ❌ No | No distributed coordination |

## Implementation Options for Server-to-Server Communication

### **Option 1: Native Dual-Mode Architecture Modification**

#### **Advantages**
- Single driver instance handles both server and client operations
- Seamless integration with TensorStore APIs
- Efficient resource utilization
- Natural distributed storage abstraction

#### **Required Changes**

##### A. **Specification Enhancement**
```cpp
// Current (mutually exclusive):
struct RemoteDramDriverSpecData {
  std::optional<std::string> listen_addr;   // Server mode
  std::optional<std::string> remote_addr;   // Client mode
};

// Proposed (dual mode):
struct RemoteDramDriverSpecData {
  std::optional<std::string> listen_addr;              // Server capability
  std::vector<std::string> remote_addrs;               // Multiple client capabilities  
  std::optional<std::string> key_routing_strategy;     // "hash", "range", "explicit"
  bool enable_server_to_server = false;                // Feature flag
  std::unordered_map<std::string, std::string> key_to_server_mapping;  // Explicit routing
};
```

##### B. **Driver State Refactoring**
```cpp
class RemoteDramDriver {
  // Current:
  bool is_server_mode_;
  ucp_ep_h client_endpoint_;
  
  // Proposed:
  bool has_server_capability_;
  bool has_client_capability_;
  std::unordered_map<std::string, ucp_ep_h> server_endpoints_;  // Multiple server connections
  std::unique_ptr<KeyRouter> key_router_;                       // Key-to-server routing logic
};
```

##### C. **Key Routing Implementation**
```cpp
class KeyRouter {
public:
  virtual ~KeyRouter() = default;
  virtual std::string SelectServerForKey(const kvstore::Key& key) = 0;
  virtual bool IsLocalKey(const kvstore::Key& key) = 0;
};

class HashBasedRouter : public KeyRouter {
  std::vector<std::string> server_addresses_;
  std::string local_address_;
public:
  std::string SelectServerForKey(const kvstore::Key& key) override {
    size_t hash = std::hash<std::string>{}(std::string(key));
    return server_addresses_[hash % server_addresses_.size()];
  }
  
  bool IsLocalKey(const kvstore::Key& key) override {
    return SelectServerForKey(key) == local_address_;
  }
};
```

##### D. **Read/Write Logic Enhancement**
```cpp
Future<kvstore::ReadResult> Read(Key key, ReadOptions options) override {
  if (has_server_capability_ && key_router_->IsLocalKey(key)) {
    return ReadLocal(key, options);
  } else if (has_client_capability_) {
    auto target_server = key_router_->SelectServerForKey(key);
    auto endpoint_it = server_endpoints_.find(target_server);
    if (endpoint_it != server_endpoints_.end()) {
      return ReadFromServer(endpoint_it->second, key, options);
    } else {
      return absl::NotFoundError(absl::StrFormat("No connection to server: %s", target_server));
    }
  } else {
    return absl::InvalidArgumentError("No capability to handle key");
  }
}
```

#### **Implementation Complexity**: **High**
- Requires significant refactoring of driver logic
- Need to implement key routing strategies
- Complex endpoint management for multiple connections
- Extensive testing for distributed scenarios

### **Option 2: Separate Driver Instances (Recommended)**

#### **Advantages**
- **Minimal code changes** - works within current architecture
- **Clear separation of concerns** - server vs client responsibilities
- **Immediate implementability** - no architectural refactoring needed
- **Flexible deployment** - can run different combinations

#### **Architecture Pattern**
```cpp
class DistributedServerNode {
private:
  kvstore::KvStore server_store_;      // Handle incoming requests
  std::unordered_map<std::string, kvstore::KvStore> client_stores_;  // Outbound connections
  
public:
  absl::Status Initialize(const std::string& listen_addr, 
                         const std::vector<std::string>& peer_addrs) {
    // Server capability
    auto server_result = tensorstore::kvstore::Open({
      {"driver", "remote_dram"},
      {"listen_addr", listen_addr}
    }).result();
    
    if (!server_result.ok()) return server_result.status();
    server_store_ = *server_result;
    
    // Client capabilities to other servers
    for (const auto& peer_addr : peer_addrs) {
      auto client_result = tensorstore::kvstore::Open({
        {"driver", "remote_dram"},
        {"remote_addr", peer_addr}
      }).result();
      
      if (!client_result.ok()) return client_result.status();
      client_stores_[peer_addr] = *client_result;
    }
    
    return absl::OkStatus();
  }
  
  Future<void> WriteToServer(const std::string& server_addr, 
                           const kvstore::Key& key, 
                           const absl::Cord& value) {
    auto it = client_stores_.find(server_addr);
    if (it != client_stores_.end()) {
      return tensorstore::kvstore::Write(it->second, key, value);
    } else {
      return absl::NotFoundError("No connection to server");
    }
  }
  
  Future<kvstore::ReadResult> ReadFromServer(const std::string& server_addr,
                                           const kvstore::Key& key) {
    auto it = client_stores_.find(server_addr);
    if (it != client_stores_.end()) {
      return tensorstore::kvstore::Read(it->second, key);
    } else {
      return MakeReadyFuture<kvstore::ReadResult>(
          kvstore::ReadResult::Missing(absl::Now()));
    }
  }
};
```

#### **Usage Example**
```cpp
// Node 1 setup (server + clients to other nodes)
DistributedServerNode node1;
node1.Initialize("0.0.0.0:12345", {
  "192.168.1.100:12345",  // Node 2
  "192.168.1.101:12345"   // Node 3
});

// Write local data
tensorstore::kvstore::Write(node1.GetServerStore(), "local_key", absl::Cord("local_data"));

// Write to remote server
node1.WriteToServer("192.168.1.100:12345", "remote_key", absl::Cord("remote_data"));

// Read from remote server  
auto result = node1.ReadFromServer("192.168.1.101:12345", "some_key").result();
```

#### **Implementation Complexity**: **Low**
- Uses existing driver architecture without modification
- Straightforward connection management
- Easy to implement and test
- Can be deployed immediately

### **Option 3: External Bridge/Coordinator**

#### **Architecture**
- Keep current single-role remote_dram drivers unchanged
- Implement separate coordination service for server-to-server communication
- Use message queues, REST APIs, or other transport mechanisms

#### **Advantages**
- **No changes to remote_dram implementation**
- **Technology flexibility** - can use any transport (HTTP, gRPC, message queues)
- **Loose coupling** - servers remain independent

#### **Disadvantages**
- **Additional complexity** - separate service to manage
- **Network overhead** - extra hop for server-to-server operations
- **Consistency challenges** - harder to maintain ACID properties

## Recommended Implementation Approach

### **Phase 2A: Immediate Solution (Option 2)**

**Goal**: Enable server-to-server communication with minimal code changes

**Timeline**: 1-2 weeks

**Tasks**:
1. Create `DistributedServerNode` wrapper class
2. Implement connection management for multiple client stores
3. Add configuration support for peer server addresses
4. Create examples demonstrating 3-node cluster communication
5. Add comprehensive testing for distributed scenarios

**Deliverables**:
- Working multi-server communication
- Example applications showing data distribution
- Documentation for distributed deployment

### **Phase 2B: Long-term Architecture (Option 1)**

**Goal**: Native distributed storage with automatic key routing

**Timeline**: 4-6 weeks

**Tasks**:
1. Refactor driver specification to support dual-mode
2. Implement key routing strategies (hash-based, range-based)
3. Add connection pool management for multiple server endpoints
4. Implement distributed consistency protocols
5. Add fault tolerance and reconnection logic
6. Performance optimization for distributed operations

**Deliverables**:
- Seamless distributed kvstore with TensorStore integration
- Multiple key distribution strategies
- Fault-tolerant distributed storage system
- Performance benchmarks for distributed operations

## Use Cases Enabled

### **Distributed TensorStore Operations**
```python
# Using distributed remote_dram as backend
import tensorstore as ts

# Opens distributed array across multiple nodes
dataset = ts.open({
    'driver': 'zarr',
    'kvstore': {
        'driver': 'remote_dram',
        'listen_addr': '0.0.0.0:12345',
        'remote_addrs': ['node2:12345', 'node3:12345'],
        'key_routing': 'hash'
    }
}).result()

# Writes are automatically distributed
dataset[0:1000, 0:1000] = data  # May span multiple servers
```

### **Multi-Node Data Pipeline**
- **Node 1**: Collects and preprocesses data → writes to local storage
- **Node 2**: Reads from Node 1 → processes → writes results to Node 3  
- **Node 3**: Aggregates results from multiple nodes → serves final data

### **Fault-Tolerant Distributed Storage**
- **Data replication** across multiple server nodes
- **Automatic failover** when server nodes become unavailable
- **Load balancing** for read operations across replicas

## Testing Strategy

### **Phase 2A Testing**
1. **Unit Tests**: `DistributedServerNode` connection management
2. **Integration Tests**: 3-node cluster setup with data transfer
3. **Performance Tests**: Latency and throughput for server-to-server operations
4. **Failure Tests**: Network partitions and server failures

### **Phase 2B Testing**  
1. **Distributed Consistency Tests**: Concurrent operations across nodes
2. **Key Distribution Tests**: Verify routing strategies work correctly
3. **Scale Tests**: Performance with 10+ nodes
4. **TensorStore Integration Tests**: End-to-end distributed array operations

## Migration Path

### **Current State → Phase 2A**
- **No breaking changes** to existing single-server deployments
- **Additive functionality** - existing code continues to work
- **Optional feature** - distributed mode is opt-in

### **Phase 2A → Phase 2B**
- **Gradual migration** - can run both architectures simultaneously
- **Configuration-driven** - same codebase supports both modes
- **Backward compatibility** - existing distributed setups continue working

## Success Criteria

### **Phase 2A Success**
- ✅ Server node can simultaneously listen for clients AND connect to other servers
- ✅ Data can be transferred between server nodes programmatically
- ✅ Examples demonstrate 3+ node data distribution scenarios
- ✅ Performance is acceptable for distributed use cases

### **Phase 2B Success**
- ✅ Single TensorStore kvstore handle provides transparent distributed storage
- ✅ Automatic key routing distributes data across nodes efficiently
- ✅ System gracefully handles node failures and network partitions
- ✅ Performance scales linearly with number of nodes

---

## Next Steps

1. **Review and prioritize** Phase 2A vs Phase 2B approach based on use case urgency
2. **Define specific distributed scenarios** that need to be supported
3. **Create implementation plan** with detailed task breakdown
4. **Set up development environment** for multi-node testing
5. **Begin implementation** of chosen approach

---
*Document Status: Phase 2 Planning Complete*  
*Next Phase: Implementation Planning and Resource Allocation*  
*Generated: 2025-01-14*