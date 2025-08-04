# Migration Strategy: UCX Network Layer Separation

## Overview

This document outlines the strategy for migrating the existing Remote DRAM KVStore implementation to use the new separated architecture with clean boundaries between network (UCX) and application layers.

## Current Implementation Issues

The current implementation violates separation of concerns by having UCX callbacks perform:
- Message parsing and validation
- Storage operations
- Promise/future management
- Protocol handling

This causes:
- High callback latency (500μs target → 25μs)
- Thread contention
- Poor error isolation
- Limited scalability

## New Architecture Components

### 1. Network Event System (`network_event.h`)
- Clean event abstraction for network completions
- Event types: DATA_RECEIVED, DATA_SENT, CONNECTION_ESTABLISHED, etc.
- No application logic

### 2. Pure UCX Network Layer (`ucx_network_layer.h/cc`)
- Only handles UCX operations
- Clean callbacks that only notify about network events
- No message parsing or application logic
- Target callback latency: <25μs

### 3. Application Message Handler (`application_message_handler.h/cc`)
- Handles all Remote DRAM business logic
- Message parsing and validation
- Storage operations
- Async operation management
- Protocol handling

### 4. Network-Application Bridge (`network_application_bridge.h/cc`)
- Minimal coordination layer
- Clean interface between network and application
- No mixing of concerns

## Migration Steps

### Phase 1: Parallel Implementation (Current)
1. ✅ Implement new separated components alongside existing code
2. ✅ Ensure new components compile and pass basic tests
3. ✅ No changes to existing functionality

### Phase 2: Gradual Integration
1. Update `UcxManager` to optionally use new network layer
2. Add feature flag to enable separated architecture
3. Create adapter to use new components with existing API
4. Run parallel testing with both implementations

### Phase 3: Full Migration
1. Replace existing UCX callbacks with new architecture
2. Update all client code to use bridge interface
3. Remove old callback implementations
4. Performance validation

## Integration Points

### 1. UcxManager Update
```cpp
class UcxManager {
private:
    // Add new components
    std::unique_ptr<UcxNetworkLayer> network_layer_;
    std::unique_ptr<application::RemoteDramMessageHandler> app_handler_;
    std::unique_ptr<NetworkApplicationBridge> bridge_;
    
    // Feature flag
    bool use_separated_architecture_ = false;
    
public:
    void EnableSeparatedArchitecture() {
        use_separated_architecture_ = true;
        InitializeSeparatedComponents();
    }
};
```

### 2. Callback Migration
Replace existing callbacks like:
```cpp
void ServerReceiveCallback(void* request, ucs_status_t status, 
                          const ucp_tag_recv_info_t* info, void* user_data) {
    // Current: 100+ lines of application logic
}
```

With pure network callbacks:
```cpp
void UcxNetworkLayer::PureReceiveCallback(void* request, ucs_status_t status,
                                         const ucp_tag_recv_info_t* info, void* user_data) {
    // Only create network event and notify (5-10 lines)
}
```

### 3. Message Handling Migration
Move all message processing to application layer:
```cpp
void RemoteDramMessageHandler::HandleReceivedData(const network::NetworkEvent& event) {
    // Application handles its own message parsing, storage, responses
}
```

## Testing Strategy

### 1. Unit Tests
- ✅ Network layer isolation tests (`ucx_network_layer_test.cc`)
- Application logic tests (TODO)
- Integration tests (TODO)

### 2. Performance Tests
- Callback latency measurement (target <25μs)
- Throughput comparison (maintain current performance)
- Concurrency testing

### 3. Compatibility Tests
- Ensure existing TensorStore API works unchanged
- Test with existing client applications
- Verify backward compatibility

## Risk Mitigation

### 1. Performance Risks
- **Risk**: Separation overhead reduces throughput
- **Mitigation**: Lock-free event queues, dedicated threads
- **Validation**: Continuous benchmarking

### 2. Compatibility Risks
- **Risk**: Breaking existing functionality
- **Mitigation**: Feature flags, parallel implementation
- **Validation**: Comprehensive regression testing

### 3. Integration Risks
- **Risk**: Complex migration process
- **Mitigation**: Gradual rollout, adapter pattern
- **Validation**: Staged deployment

## Success Metrics

1. **Callback Latency**: <25μs (10x improvement)
2. **Throughput**: Maintain or improve current performance
3. **Code Quality**: Clean separation, no mixed concerns
4. **Reliability**: Better error isolation
5. **Maintainability**: Easier to test and modify

## Next Steps

1. Complete unit tests for all components
2. Implement adapter for existing API
3. Add feature flag to UcxManager
4. Begin gradual integration testing
5. Performance benchmarking
6. Documentation updates

## Timeline

- **Week 1-2**: ✅ Phase 1 (Network Event Abstraction) - COMPLETE
- **Week 3-4**: ✅ Phase 2 (Application Message Handler) - COMPLETE  
- **Week 5-6**: ✅ Phase 3 (Clean Integration Layer) - COMPLETE
- **Week 7-8**: Phase 4 (Performance Optimization) - TODO
- **Week 9-10**: Integration and migration
- **Week 11-12**: Testing and validation