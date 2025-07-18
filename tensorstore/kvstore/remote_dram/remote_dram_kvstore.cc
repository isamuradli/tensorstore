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

#include "remote_dram_kvstore.h"

#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

// System includes for socket operations
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "absl/status/status.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_binding/json_binding.h"
#include "tensorstore/internal/uri_utils.h"
#include "tensorstore/kvstore/byte_range.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/key_range.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/kvstore/read_result.h"
#include "tensorstore/kvstore/registry.h"
#include "tensorstore/kvstore/spec.h"
#include "tensorstore/kvstore/url_registry.h"
#include "tensorstore/util/execution/execution.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/str_cat.h"

// specializations for std::optional
#include "tensorstore/internal/cache_key/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/internal/json_binding/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/serialization/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/util/garbage_collection/std_optional.h"  // IWYU pragma: keep

// UCX headers
#include <ucp/api/ucp.h>

namespace tensorstore {

// RemoteDramStorage Implementation
void RemoteDramStorage::Store(const std::string& key, const absl::Cord& value) {
  absl::MutexLock lock(&mutex_);
  storage_[key] = value;
  ABSL_LOG(INFO) << "Stored key '" << key << "' with " << value.size() << " bytes";
}

std::optional<absl::Cord> RemoteDramStorage::Get(const std::string& key) const {
  absl::MutexLock lock(&mutex_);
  auto it = storage_.find(key);
  if (it != storage_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool RemoteDramStorage::Exists(const std::string& key) const {
  absl::MutexLock lock(&mutex_);
  return storage_.find(key) != storage_.end();
}

bool RemoteDramStorage::Remove(const std::string& key) {
  absl::MutexLock lock(&mutex_);
  return storage_.erase(key) > 0;
}

std::vector<std::string> RemoteDramStorage::GetAllKeys() const {
  absl::MutexLock lock(&mutex_);
  std::vector<std::string> keys;
  keys.reserve(storage_.size());
  for (const auto& [key, value] : storage_) {
    keys.push_back(key);
  }
  return keys;
}

size_t RemoteDramStorage::GetKeyCount() const {
  absl::MutexLock lock(&mutex_);
  return storage_.size();
}

/// UCX error handler callback
void UcxErrorHandler(void* arg, ucp_ep_h ep, ucs_status_t status) {
  ABSL_LOG(ERROR) << "UCX: Connection error: " << ucs_status_string(status);
}

namespace {

namespace jb = tensorstore::internal_json_binding;

/// Send completion callback for UCX operations
void SendCallback(void* request, ucs_status_t status, void* user_data) {
  // Handle the new user_data structure
  struct SendUserData {
    uint64_t request_id;
    char* buffer_to_free;
  };
  
  SendUserData* data = static_cast<SendUserData*>(user_data);
  uint64_t request_id = data->request_id;
  char* buffer_to_free = data->buffer_to_free;
  
  ABSL_LOG(INFO) << "UCX send completed for request " << request_id 
                 << " with status: " << ucs_status_string(status);
  
  if (status == UCS_OK) {
    std::cout << "✅ UCX: Send completed successfully [ID:" << request_id << "]" << std::endl;
  } else {
    std::cout << "❌ UCX: Send failed [ID:" << request_id << "] - " << ucs_status_string(status) << std::endl;
  }
  
  absl::Status result_status = (status == UCS_OK) ? 
    absl::OkStatus() : 
    absl::InternalError(absl::StrFormat("UCX send failed: %s", ucs_status_string(status)));
  
  // Schedule completion to avoid deadlock in callback
  std::thread([request_id, result_status, buffer_to_free, data]() {
    UcxManager::Instance().CompletePendingOperation(request_id, result_status);
    // Free the buffer and user_data after completing the operation
    delete[] buffer_to_free;
    delete data;
  }).detach();
  
  ucp_request_free(request);
}

/// Connection callback for server-side client connections
void UcxListenerCallback(ucp_conn_request_h conn_request, void* user_data) {
  ABSL_LOG(INFO) << "UCX listener received connection request";
  
  // Accept the connection request
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  ep_params.conn_request = conn_request;
  
  ucp_ep_h server_endpoint;
  ucs_status_t status = ucp_ep_create(UcxManager::Instance().GetWorker(), &ep_params, &server_endpoint);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create server endpoint: " << ucs_status_string(status);
    return;
  }
  
  ABSL_LOG(INFO) << "Server endpoint created successfully for incoming connection";
  
  // Register this endpoint as a client endpoint for responses
  // Schedule endpoint registration to avoid deadlock
  std::thread([server_endpoint]() {
    UcxManager::Instance().RegisterClientEndpoint(server_endpoint);
  }).detach();
}

/// Receive completion callback for server-side message handling
void ServerReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX server receive completed with status: " << ucs_status_string(status);
  
  // Always free the receive buffer, regardless of success or failure
  char* buffer = static_cast<char*>(user_data);
  
  if (status == UCS_OK && info->length >= sizeof(MessageHeader)) {
    // Log message for debugging
    message_utils::LogMessageBuffer(buffer, info->length, "Server received message");
    
    // Process the received message
    MessageHeader* header = reinterpret_cast<MessageHeader*>(buffer);
    
    // Verify message integrity
    if (!message_utils::VerifyMessageHeader(header, info->length)) {
      ABSL_LOG(ERROR) << "Message integrity verification failed, discarding message";
      delete[] buffer;
      if (request != nullptr) {
        ucp_request_free(request);
      }
      return;
    }
    
    size_t header_offset = 0;  // No offset needed with proper buffer management
    
    ABSL_LOG(INFO) << "Received message: magic=" << std::dec << header->magic_number
                   << ", type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length 
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id
                   << ", checksum=" << header->checksum;
    
    // Try to extract readable content from the message payload
    // Skip the header and look for the actual key-value data
    if (info->length > sizeof(MessageHeader)) {
      const char* payload = buffer + sizeof(MessageHeader);
      size_t payload_size = info->length - sizeof(MessageHeader);
      
      // Look for the key-value pattern: should be "node1_test" followed by "Data from Node 1"
      std::string payload_str(payload, payload_size);
      ABSL_LOG(INFO) << "Raw payload content: '" << payload_str << "'";
      
      // Try to find readable strings in the payload
      std::string readable_payload;
      for (size_t i = 0; i < payload_size; ++i) {
        char c = payload[i];
        if (c >= 32 && c <= 126) {  // Printable ASCII
          readable_payload += c;
        } else if (c == 0) {
          readable_payload += " [NULL] ";
        } else {
          readable_payload += " [" + std::to_string(static_cast<unsigned char>(c)) + "] ";
        }
      }
      ABSL_LOG(INFO) << "Readable payload: " << readable_payload;
      
      // Try to find the expected key and value strings in the buffer
      std::string expected_key = "node1_test";
      std::string expected_value = "Data from Node 1";
      
      // Search for the key string
      std::string entire_buffer(buffer, info->length);
      size_t key_pos = entire_buffer.find(expected_key);
      size_t value_pos = entire_buffer.find(expected_value);
      
      if (key_pos != std::string::npos) {
        ABSL_LOG(INFO) << "Found expected key '" << expected_key << "' at position " << key_pos;
      }
      if (value_pos != std::string::npos) {
        ABSL_LOG(INFO) << "Found expected value '" << expected_value << "' at position " << value_pos;
      }
      
      if (key_pos != std::string::npos && value_pos != std::string::npos) {
        ABSL_LOG(INFO) << "SUCCESS: Both key and value found in received buffer!";
        ABSL_LOG(INFO) << "  Key: '" << expected_key << "' at position " << key_pos;
        ABSL_LOG(INFO) << "  Value: '" << expected_value << "' at position " << value_pos;
      }
    }
    
    if (header->type == MessageType::WRITE_REQUEST) {
      // Handle write request
      const char* data_ptr = buffer + header_offset + sizeof(MessageHeader);
      
      // Verify payload checksum
      if (header->checksum != 0) {
        uint32_t calculated_checksum = message_utils::CalculateChecksum(data_ptr, 
                                                                       header->key_length + header->value_length);
        if (calculated_checksum != header->checksum) {
          ABSL_LOG(ERROR) << "Payload checksum mismatch: expected " << std::dec << header->checksum
                          << ", calculated " << std::dec << calculated_checksum;
          delete[] buffer;
          if (request != nullptr) {
            ucp_request_free(request);
          }
          return;
        }
      }
      
      std::string key(data_ptr, header->key_length);
      absl::Cord value(std::string(data_ptr + header->key_length, header->value_length));
      
      // Store in server memory
      UcxManager::Instance().GetStorage().Store(key, value);
      
      ABSL_LOG(INFO) << "Server stored key-value pair: key='" << key 
                     << "', value_size=" << value.size();
      
      std::cout << "📥 SERVER: Received write request [ID:" << header->request_id << "] key='" << key 
                << "' value='" << std::string(value) << "' - STORED SUCCESS" << std::endl;
      
      // Send write response back to client
      // Schedule response sending to avoid deadlock in callback
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      std::thread([request_id, key_copy]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendWriteResponse(client_endpoint, request_id, 0);  // 0 = success
          ABSL_LOG(INFO) << "Sent write response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send write response for key: " << key_copy;
        }
      }).detach();
      
    } else if (header->type == MessageType::READ_REQUEST) {
      // Handle read request
      const char* data_ptr = buffer + header_offset + sizeof(MessageHeader);
      std::string key(data_ptr, header->key_length);
      
      ABSL_LOG(INFO) << "Server received read request for key='" << key << "'";
      
      std::cout << "📬 SERVER: Received read request [ID:" << header->request_id << "] key='" << key << "'" << std::endl;
      
      // Look up the key in storage
      auto& storage = UcxManager::Instance().GetStorage();
      auto value = storage.Get(key);
      
      if (value.has_value()) {
        ABSL_LOG(INFO) << "Server found key '" << key << "' with value size=" << value->size();
        std::cout << "📤 SERVER: Found key '" << key << "' [ID:" << header->request_id << "] - sending response with " << value->size() << " bytes" << std::endl;
      } else {
        ABSL_LOG(INFO) << "Server key '" << key << "' not found";
        std::cout << "❌ SERVER: Key '" << key << "' not found [ID:" << header->request_id << "]" << std::endl;
      }
      
      // Send read response back to client
      // Schedule response sending to avoid deadlock in callback
      uint64_t request_id = header->request_id;
      std::string key_copy = key;
      std::thread([request_id, key_copy, value]() {
        auto& ucx_manager = UcxManager::Instance();
        ucp_ep_h client_endpoint = ucx_manager.GetClientEndpoint();
        if (client_endpoint) {
          ucx_manager.SendReadResponse(client_endpoint, request_id, value);
          ABSL_LOG(INFO) << "Sent read response for key: " << key_copy;
        } else {
          ABSL_LOG(ERROR) << "No client endpoint available to send read response for key: " << key_copy;
        }
      }).detach();
    }
    
    // Only post another receive buffer if we're not shutting down
    // Check if UCX manager is still active before posting new receive
    auto& ucx_manager = UcxManager::Instance();
    if (ucx_manager.GetContext() && ucx_manager.GetWorker()) {
      // Schedule posting to avoid deadlock in callback
      std::thread([&ucx_manager]() {
        ucx_manager.PostServerReceive();
      }).detach();
    }
  } else {
    ABSL_LOG(ERROR) << "Failed to receive message: " << ucs_status_string(status);
  }
  
  // Free the receive buffer
  delete[] buffer;
  
  // Free the UCX request
  if (request != nullptr) {
    ucp_request_free(request);
  }
}

/// Client-side callback for handling read responses
void ClientReceiveCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX client receive completed with status: " << ucs_status_string(status);
  
  // Extract user data structure
  struct ReadUserData {
    uint64_t request_id;
    char* buffer;
  };
  ReadUserData* read_data = static_cast<ReadUserData*>(user_data);
  uint64_t request_id = read_data->request_id;
  char* buffer = read_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(ReadResponse)) {
    // The data was received into the buffer we provided
    // Copy to ensure we have valid data access
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content and hex dump
    ABSL_LOG(INFO) << "Client received " << info->length << " bytes";
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received read response");
    
    // Add hex dump for debugging the exact buffer layout
    std::stringstream hex_dump;
    for (size_t i = 0; i < std::min(info->length, static_cast<size_t>(64)); ++i) {
      hex_dump << std::hex << std::setfill('0') << std::setw(2) 
               << static_cast<unsigned char>(response_data[i]) << " ";
      if ((i + 1) % 16 == 0) hex_dump << "\n";
    }
    ABSL_LOG(INFO) << "Hex dump of received data:\n" << hex_dump.str();
    
    ReadResponse* response = reinterpret_cast<ReadResponse*>(response_data.data());
    
    ABSL_LOG(INFO) << "Client received read response: status_code=" << response->status_code
                   << ", value_len=" << response->header.value_length
                   << ", request_id=" << response->header.request_id;
    
    kvstore::ReadResult result;
    if (response->status_code == 0 && response->header.value_length > 0) {
      // Validate value_length is reasonable
      if (response->header.value_length > 1000000) {  // 1MB max sanity check
        ABSL_LOG(ERROR) << "Received invalid value_length: " << response->header.value_length;
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
      } else if (info->length < sizeof(ReadResponse) + response->header.value_length) {
        ABSL_LOG(ERROR) << "Received message too small for claimed value_length";
        result.state = kvstore::ReadResult::kMissing;
        result.stamp.generation = StorageGeneration::NoValue();
        result.stamp.time = absl::Now();
      } else {
        // Success with value
        result.state = kvstore::ReadResult::kValue;
        const char* value_ptr = response_data.data() + sizeof(ReadResponse);
        result.value = absl::Cord(std::string(value_ptr, response->header.value_length));
        result.stamp.generation = StorageGeneration::FromString("remote_read");
        result.stamp.time = absl::Now();
        ABSL_LOG(INFO) << "Read successful, value size=" << result.value.size();
        std::cout << "📥 CLIENT: Read response [ID:" << response->header.request_id << "] SUCCESS - received " << result.value.size() << " bytes" << std::endl;
      }
    } else {
      // Key not found or error
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      ABSL_LOG(INFO) << "Read result: key not found";
      std::cout << "❌ CLIENT: Read response [ID:" << response->header.request_id << "] KEY NOT FOUND" << std::endl;
    }
    
    // Complete the pending read operation
    // Schedule completion to avoid deadlock in callback
    std::thread([request_id, result = std::move(result)]() mutable {
      UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(result));
    }).detach();
    
    delete[] buffer;
    delete read_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive read response: " << ucs_status_string(status);
    
    // Complete with error
    // Schedule completion to avoid deadlock in callback
    std::thread([request_id]() {
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      UcxManager::Instance().CompletePendingReadOperation(request_id, std::move(error_result));
    }).detach();
    
    delete[] buffer;
    delete read_data;
  }
  
  ucp_request_free(request);
}

/// Client-side callback for handling write responses
void ClientWriteResponseCallback(void* request, ucs_status_t status, const ucp_tag_recv_info_t* info, void* user_data) {
  ABSL_LOG(INFO) << "UCX client write response received with status: " << ucs_status_string(status);
  
  // Extract user data structure (same pattern as read)
  struct WriteUserData {
    uint64_t request_id;
    char* buffer;
  };
  WriteUserData* write_data = static_cast<WriteUserData*>(user_data);
  uint64_t request_id = write_data->request_id;
  char* buffer = write_data->buffer;
  
  if (status == UCS_OK && info->length >= sizeof(WriteResponse)) {
    // The data was received into the buffer we provided
    // Copy to ensure we have valid data access (same pattern as read)
    std::vector<char> response_data(info->length);
    std::memcpy(response_data.data(), buffer, info->length);
    
    // Debug: Log the raw buffer content
    ABSL_LOG(INFO) << "Client received write response " << info->length << " bytes";
    message_utils::LogMessageBuffer(response_data.data(), info->length, "Client received write response");
    
    WriteResponse* response = reinterpret_cast<WriteResponse*>(response_data.data());
    
    ABSL_LOG(INFO) << "Client received write response: status_code=" << response->status_code
                   << ", request_id=" << response->header.request_id;
    
    absl::Status result_status;
    if (response->status_code == 0) {
      // Success
      result_status = absl::OkStatus();
      ABSL_LOG(INFO) << "Write successful for request_id=" << response->header.request_id;
      std::cout << "📥 CLIENT: Write response [ID:" << response->header.request_id << "] SUCCESS" << std::endl;
    } else {
      // Error
      result_status = absl::InternalError("Write failed on server");
      ABSL_LOG(INFO) << "Write failed for request_id=" << response->header.request_id;
      std::cout << "❌ CLIENT: Write response [ID:" << response->header.request_id << "] FAILED" << std::endl;
    }
    
    // Complete the pending write operation
    // Schedule completion to avoid deadlock in callback
    std::thread([request_id, result_status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, result_status);
    }).detach();
    
    delete[] buffer;
    delete write_data;
  } else {
    ABSL_LOG(ERROR) << "Failed to receive write response: " << ucs_status_string(status);
    
    // Complete with error
    // Schedule completion to avoid deadlock in callback
    std::thread([request_id, status]() {
      UcxManager::Instance().CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("Failed to receive write response: %s", 
                                             ucs_status_string(status))));
    }).detach();
    
    delete[] buffer;
    delete write_data;
  }
  
  ucp_request_free(request);
}


/// Send a notification to the server process that new data has been written
void NotifyServerOfNewData(const kvstore::Key& key, const absl::Cord& value) {
  // Create a TCP socket to notify the server
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    ABSL_LOG(WARNING) << "Failed to create notification socket";
    return;
  }
  
  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  
  // Connect to server notification port (12346 = data port + 1)
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(12346);
  server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  
  if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ABSL_LOG(WARNING) << "Failed to connect to server notification port 12346";
    close(sock);
    return;
  }
  
  // Send notification message with length prefix for reliable parsing
  std::string key_str = std::string(key);
  std::string value_str = std::string(value);
  
  // Format: "NEW_DATA:<key_length>:<value_length>:<key><value>"
  std::string notification = "NEW_DATA:" + std::to_string(key_str.length()) + ":" + 
                           std::to_string(value_str.length()) + ":" + key_str + value_str;
  
  ABSL_LOG(INFO) << "Sending notification to server: key='" << key_str << "', value_size=" << value_str.length();
  
  ssize_t bytes_sent = send(sock, notification.c_str(), notification.length(), 0);
  if (bytes_sent != static_cast<ssize_t>(notification.length())) {
    ABSL_LOG(WARNING) << "Failed to send complete notification to server";
  } else {
    ABSL_LOG(INFO) << "Notification sent successfully to server";
  }
  
  close(sock);
}

}  // namespace

// Message utility functions implementation
namespace message_utils {

uint32_t CalculateChecksum(const void* data, size_t size) {
  uint32_t checksum = 0;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    checksum = (checksum << 1) ^ bytes[i];
  }
  return checksum;
}

bool VerifyMessageHeader(const MessageHeader* header, size_t total_message_size) {
  if (!header) {
    ABSL_LOG(ERROR) << "Message header is null";
    return false;
  }
  
  // Check magic number
  if (header->magic_number != MESSAGE_MAGIC_NUMBER) {
    ABSL_LOG(ERROR) << "Invalid magic number: expected " << std::dec << MESSAGE_MAGIC_NUMBER
                    << ", got " << std::dec << header->magic_number;
    return false;
  }
  
  // Check message type
  if (header->type != MessageType::WRITE_REQUEST && 
      header->type != MessageType::WRITE_RESPONSE &&
      header->type != MessageType::READ_REQUEST &&
      header->type != MessageType::READ_RESPONSE) {
    ABSL_LOG(ERROR) << "Invalid message type: " << static_cast<uint32_t>(header->type);
    return false;
  }
  
  // Check total message size consistency
  size_t expected_size = sizeof(MessageHeader) + header->key_length + header->value_length;
  if (total_message_size < expected_size) {
    ABSL_LOG(ERROR) << "Message size mismatch: expected at least " << expected_size
                    << " bytes, got " << total_message_size;
    return false;
  }
  
  return true;
}

void InitializeMessageHeader(MessageHeader* header, MessageType type, 
                            uint32_t key_length, uint32_t value_length, 
                            uint64_t request_id, const void* payload_data) {
  header->magic_number = MESSAGE_MAGIC_NUMBER;
  header->type = type;
  header->key_length = key_length;
  header->value_length = value_length;
  header->request_id = request_id;
  
  // Calculate checksum for payload data
  if (payload_data && (key_length + value_length) > 0) {
    header->checksum = CalculateChecksum(payload_data, key_length + value_length);
  } else {
    header->checksum = 0;
  }
}

void LogMessageBuffer(const char* buffer, size_t size, const std::string& prefix) {
  ABSL_LOG(INFO) << prefix << " (" << size << " bytes)";
  
  // Log the message header interpretation if present
  if (size >= sizeof(MessageHeader)) {
    const MessageHeader* header = reinterpret_cast<const MessageHeader*>(buffer);
    ABSL_LOG(INFO) << "  Header: magic=" << std::dec << header->magic_number
                   << ", type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id
                   << ", checksum=" << header->checksum;
  }
  
  // Extract and display the readable payload content
  if (size > sizeof(MessageHeader)) {
    const char* payload = buffer + sizeof(MessageHeader);
    size_t payload_size = size - sizeof(MessageHeader);
    
    std::string readable_payload;
    for (size_t i = 0; i < payload_size && i < 256; ++i) {  // Limit to 256 chars
      char c = payload[i];
      if (c >= 32 && c <= 126) {  // Printable ASCII
        readable_payload += c;
      } else if (c == 0) {
        readable_payload += "<NULL>";
      } else {
        readable_payload += "<" + std::to_string(static_cast<unsigned char>(c)) + ">";
      }
    }
    
    if (payload_size > 256) {
      readable_payload += "...";
    }
    
    ABSL_LOG(INFO) << "  Payload: " << readable_payload;
  }
}

}  // namespace message_utils

// UcxManager Implementation
UcxManager& UcxManager::Instance() {
  static UcxManager instance;
  return instance;
}

absl::Status UcxManager::Initialize() {
  absl::MutexLock lock(&mutex_);
  
  if (initialized_) {
    return absl::OkStatus();
  }
  
  // Initialize UCX context
  ucp_params_t ucp_params;
  ucp_config_t* config;
  ucs_status_t status;
  
  // Read UCX configuration
  status = ucp_config_read(nullptr, nullptr, &config);
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to read UCX config: %s", 
                                               ucs_status_string(status)));
  }
  
  // Set UCX context parameters with tagged messaging support
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | 
                          UCP_PARAM_FIELD_TAG_SENDER_MASK |
                          UCP_PARAM_FIELD_REQUEST_SIZE |
                          UCP_PARAM_FIELD_REQUEST_INIT;
  
  // Use features that support local communication (shared memory) and networking
  ucp_params.features = UCP_FEATURE_TAG | 
                        UCP_FEATURE_WAKEUP |
                        UCP_FEATURE_AM |
                        UCP_FEATURE_RMA;  // Remote Memory Access for better performance
  
  ucp_params.tag_sender_mask = 0xF000000000000000ULL;  // Use upper 4 bits for message type
  ucp_params.request_size = 0;  // Use default request size
  ucp_params.request_init = nullptr;  // No request initialization
  
  // Create UCX context
  status = ucp_init(&ucp_params, config, &context_);
  ucp_config_release(config);
  
  if (status != UCS_OK) {
    return absl::InternalError(absl::StrFormat("Failed to initialize UCX context: %s", 
                                               ucs_status_string(status)));
  }
  
  // Create UCX worker with socket support
  ucp_worker_params_t worker_params;
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE |
                             UCP_WORKER_PARAM_FIELD_CPU_MASK;
  worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
  // Use default CPU mask (all CPUs available)
  UCS_CPU_ZERO(&worker_params.cpu_mask);
  
  status = ucp_worker_create(context_, &worker_params, &worker_);
  if (status != UCS_OK) {
    ucp_cleanup(context_);
    context_ = nullptr;
    return absl::InternalError(absl::StrFormat("Failed to create UCX worker: %s", 
                                               ucs_status_string(status)));
  }
  
  initialized_ = true;
  ABSL_LOG(INFO) << "UCX Manager initialized successfully with socket support";
  
  // Start worker progress polling
  StartWorkerProgressTask();
  
  return absl::OkStatus();
}

Result<ucp_listener_h> UcxManager::CreateListener(const std::string& listen_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
  ABSL_LOG(INFO) << "Creating UCX listener for address: " << listen_addr;
  
  // Parse address and port
  size_t colon_pos = listen_addr.find(':');
  if (colon_pos == std::string::npos) {
    return absl::InvalidArgumentError("Invalid listen address format, expected host:port");
  }
  
  std::string host = listen_addr.substr(0, colon_pos);
  std::string port_str = listen_addr.substr(colon_pos + 1);
  
  // Validate port number
  int port_num;
  try {
    port_num = std::stoi(port_str);
    if (port_num <= 0 || port_num > 65535) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid port number: %d", port_num));
    }
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid port format: %s", port_str));
  }
  
  // Create listener parameters
  ucp_listener_params_t listener_params;
  struct sockaddr_in listen_sockaddr;
  
  memset(&listener_params, 0, sizeof(listener_params));
  memset(&listen_sockaddr, 0, sizeof(listen_sockaddr));
  
  listen_sockaddr.sin_family = AF_INET;
  listen_sockaddr.sin_port = htons(port_num);
  
  if (host == "0.0.0.0") {
    listen_sockaddr.sin_addr.s_addr = INADDR_ANY;
    ABSL_LOG(INFO) << "Binding to all interfaces (INADDR_ANY) on port " << port_num;
  } else if (host == "127.0.0.1" || host == "localhost") {
    listen_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ABSL_LOG(INFO) << "Binding to loopback interface on port " << port_num;
  } else {
    int inet_result = inet_pton(AF_INET, host.c_str(), &listen_sockaddr.sin_addr);
    if (inet_result != 1) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid host address: %s", host));
    }
    ABSL_LOG(INFO) << "Binding to specific interface " << host << " on port " << port_num;
  }
  
  listener_params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                               UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  listener_params.sockaddr.addr = (const struct sockaddr*)&listen_sockaddr;
  listener_params.sockaddr.addrlen = sizeof(listen_sockaddr);
  listener_params.conn_handler.cb = UcxListenerCallback;
  listener_params.conn_handler.arg = nullptr;
  
  ABSL_LOG(INFO) << "Attempting to create UCX listener with parameters configured";
  
  ucp_listener_h listener;
  ucs_status_t status = ucp_listener_create(worker_, &listener_params, &listener);
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "UCX listener creation failed with status: " << ucs_status_string(status)
                    << " (" << status << ")";
    ABSL_LOG(ERROR) << "UCX listener creation details:";
    ABSL_LOG(ERROR) << "  Address: " << listen_addr;
    ABSL_LOG(ERROR) << "  Host: " << host;
    ABSL_LOG(ERROR) << "  Port: " << port_num;
    ABSL_LOG(ERROR) << "  Worker: " << worker_;
    
    // Provide more specific error messages based on common UCX listener errors
    if (status == UCS_ERR_BUSY) {
      return absl::ResourceExhaustedError(absl::StrFormat(
          "Port %d is busy or already in use. UCX error: %s", 
          port_num, ucs_status_string(status)));
    } else if (status == UCS_ERR_UNREACHABLE) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Address %s is unreachable or invalid. UCX error: %s", 
          listen_addr, ucs_status_string(status)));
    } else if (status == UCS_ERR_UNSUPPORTED) {
      return absl::UnimplementedError(absl::StrFormat(
          "UCX listener not supported with current configuration. UCX error: %s", 
          ucs_status_string(status)));
    } else {
      return absl::InternalError(absl::StrFormat("Failed to create UCX listener: %s", 
                                                 ucs_status_string(status)));
    }
  }
  
  ABSL_LOG(INFO) << "UCX listener created successfully on " << listen_addr;
  
  // Store the listener handle for cleanup
  listener_ = listener;
  
  // Post initial receive buffers for incoming messages
  constexpr int num_preposted_receives = 10;
  ABSL_LOG(INFO) << "Posting " << num_preposted_receives << " initial receive buffers";
  for (int i = 0; i < num_preposted_receives; ++i) {
    PostServerReceiveNoLock();
  }
  
  return listener;
}

Result<ucp_ep_h> UcxManager::CreateClientEndpoint(const std::string& server_addr) {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return absl::FailedPreconditionError("UCX Manager not initialized");
  }
  
  ABSL_LOG(INFO) << "Creating UCX client endpoint to: " << server_addr;
  
  // Force real UCX networking for all connections (localhost and remote)
  // Parse server address
  size_t colon_pos = server_addr.find(':');
  if (colon_pos == std::string::npos) {
    return absl::InvalidArgumentError("Invalid server address format, expected host:port");
  }
  
  std::string host = server_addr.substr(0, colon_pos);
  std::string port_str = server_addr.substr(colon_pos + 1);
  
  // Validate port number
  int port_num;
  try {
    port_num = std::stoi(port_str);
    if (port_num <= 0 || port_num > 65535) {
      return absl::InvalidArgumentError(absl::StrFormat("Invalid port number: %d", port_num));
    }
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid port format: %s", port_str));
  }
  
  // Create socket address for server connection
  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  
  server_sockaddr.sin_family = AF_INET;
  server_sockaddr.sin_port = htons(port_num);
  
  // Convert host to IP address
  int inet_result = inet_pton(AF_INET, host.c_str(), &server_sockaddr.sin_addr);
  if (inet_result != 1) {
    return absl::InvalidArgumentError(absl::StrFormat("Invalid host address: %s", host));
  }
  
  ABSL_LOG(INFO) << "Connecting to remote host " << host << " on port " << port_num;
  
  // Create UCX endpoint parameters with socket address
  ucp_ep_params_t ep_params;
  memset(&ep_params, 0, sizeof(ep_params));
  
  // Use socket address with proper field mask
  ep_params.field_mask = UCP_EP_PARAM_FIELD_SOCK_ADDR | UCP_EP_PARAM_FIELD_FLAGS;
  ep_params.sockaddr.addr = (const struct sockaddr*)&server_sockaddr;
  ep_params.sockaddr.addrlen = sizeof(server_sockaddr);
  ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  
  ABSL_LOG(INFO) << "Attempting to create UCX client endpoint with socket connection";
  ABSL_LOG(INFO) << "  Server address: " << server_addr;
  ABSL_LOG(INFO) << "  Socket family: " << server_sockaddr.sin_family;
  ABSL_LOG(INFO) << "  Socket port: " << ntohs(server_sockaddr.sin_port);
  ABSL_LOG(INFO) << "  Worker: " << worker_;
  
  // Create the client endpoint
  ucp_ep_h client_endpoint;
  ucs_status_t status = ucp_ep_create(worker_, &ep_params, &client_endpoint);
  
  ABSL_LOG(INFO) << "UCX endpoint creation result: " << ucs_status_string(status) << " (" << status << ")";
  
  if (status != UCS_OK) {
    ABSL_LOG(ERROR) << "Failed to create UCX client endpoint: " << ucs_status_string(status);
    return absl::InternalError(absl::StrFormat(
        "Failed to create UCX client endpoint to %s: %s",
        server_addr, ucs_status_string(status)));
  }
  
  ABSL_LOG(INFO) << "UCX client endpoint created successfully to " << server_addr;
  
  // Register this endpoint for cleanup
  RegisterClientSideEndpointNoLock(client_endpoint);
  
  return client_endpoint;
}

void UcxManager::RegisterPendingOperation(uint64_t request_id, Promise<void> promise, MessageType type) {
  absl::MutexLock lock(&mutex_);
  auto op = std::make_unique<PendingWriteOperation>(request_id, std::move(promise));
  pending_write_operations_[request_id] = std::move(op);
}

void UcxManager::RegisterPendingReadOperation(uint64_t request_id, Promise<kvstore::ReadResult> promise) {
  absl::MutexLock lock(&mutex_);
  auto op = std::make_unique<PendingReadOperation>(request_id, std::move(promise));
  pending_read_operations_[request_id] = std::move(op);
}

void UcxManager::CompletePendingOperation(uint64_t request_id, absl::Status status) {
  absl::MutexLock lock(&mutex_);
  auto it = pending_write_operations_.find(request_id);
  if (it != pending_write_operations_.end()) {
    if (status.ok()) {
      it->second->promise.SetResult(absl::OkStatus());
    } else {
      it->second->promise.SetResult(status);
    }
    pending_write_operations_.erase(it);
  }
}

void UcxManager::CompletePendingReadOperation(uint64_t request_id, kvstore::ReadResult result) {
  absl::MutexLock lock(&mutex_);
  auto it = pending_read_operations_.find(request_id);
  if (it != pending_read_operations_.end()) {
    it->second->promise.SetResult(std::move(result));
    pending_read_operations_.erase(it);
  }
}

uint64_t UcxManager::GenerateRequestId() {
  absl::MutexLock lock(&mutex_);
  return next_request_id_++;
}

void UcxManager::RegisterClientEndpoint(ucp_ep_h client_endpoint) {
  absl::MutexLock lock(&mutex_);
  client_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client endpoint, total clients: " << client_endpoints_.size();
}

void UcxManager::RegisterServerEndpoint(ucp_ep_h server_endpoint) {
  absl::MutexLock lock(&mutex_);
  server_endpoints_.push_back(server_endpoint);
  ABSL_LOG(INFO) << "Registered server endpoint, total server endpoints: " << server_endpoints_.size();
}

void UcxManager::RegisterClientSideEndpoint(ucp_ep_h client_endpoint) {
  absl::MutexLock lock(&mutex_);
  client_side_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client-side endpoint for cleanup";
}

void UcxManager::RegisterClientSideEndpointNoLock(ucp_ep_h client_endpoint) {
  // This method assumes the mutex is already held
  client_side_endpoints_.push_back(client_endpoint);
  ABSL_LOG(INFO) << "Registered client-side endpoint for cleanup";
}

ucp_ep_h UcxManager::GetClientEndpoint() const {
  absl::MutexLock lock(&mutex_);
  if (client_endpoints_.empty()) {
    return nullptr;
  }
  // Return the most recently connected client
  return client_endpoints_.back();
}

void UcxManager::PostServerReceive() {
  // Allocate buffer for incoming message
  constexpr size_t max_message_size = 64 * 1024;  // 64KB max message
  char* recv_buffer = new char[max_message_size];
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ServerReceiveCallback;
  recv_params.user_data = recv_buffer;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
  ucp_worker_h worker_handle;
  {
    absl::MutexLock lock(&mutex_);
    worker_handle = worker_;
  }
  
  if (!worker_handle) {
    ABSL_LOG(ERROR) << "Cannot post server receive: worker is null";
    delete[] recv_buffer;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_handle, recv_buffer, max_message_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    delete[] recv_buffer;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    ABSL_LOG(INFO) << "Posted server receive buffer";
    
    // Track the request for cleanup
    absl::MutexLock lock(&mutex_);
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    delete[] recv_buffer;
  }
}

void UcxManager::PostServerReceiveNoLock() {
  // Allocate buffer for incoming message
  constexpr size_t max_message_size = 64 * 1024;  // 64KB max message
  char* recv_buffer = new char[max_message_size];
  
  // Post non-blocking receive
  ucp_request_param_t recv_params;
  recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  recv_params.cb.recv = ServerReceiveCallback;
  recv_params.user_data = recv_buffer;
  
  ucp_tag_t tag = 0;  // Accept any tag for now
  ucp_tag_t tag_mask = 0;  // Accept any tag - server handles all message types
  
  if (!worker_) {
    ABSL_LOG(ERROR) << "Cannot post server receive: worker is null";
    delete[] recv_buffer;
    return;
  }
  
  void* request = ucp_tag_recv_nbx(worker_, recv_buffer, max_message_size, 
                                   tag, tag_mask, &recv_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to post server receive: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
    delete[] recv_buffer;
  } else if (request != nullptr) {
    // Request is in progress, will complete asynchronously
    ABSL_LOG(INFO) << "Posted server receive buffer";
    
    // Track the request for cleanup (mutex already held)
    active_requests_.push_back(request);
  } else {
    // Receive completed immediately (unlikely)
    delete[] recv_buffer;
  }
}

void UcxManager::CancelPendingReceives() {
  absl::MutexLock lock(&mutex_);
  CancelPendingReceivesNoLock();
}

void UcxManager::CancelPendingReceivesNoLock() {
  // This method assumes the mutex is already held
  ABSL_LOG(INFO) << "Canceling " << active_requests_.size() << " pending UCX requests";
  
  // Cancel all active requests
  for (void* request : active_requests_) {
    if (request != nullptr) {
      ucp_request_cancel(worker_, request);
    }
  }
  
  // Clear the list - the requests will be freed in their callbacks
  active_requests_.clear();
}

void UcxManager::CleanupListener() {
  absl::MutexLock lock(&mutex_);
  CleanupListenerNoLock();
}

void UcxManager::CleanupListenerNoLock() {
  // This method assumes the mutex is already held
  if (listener_ != nullptr) {
    ABSL_LOG(INFO) << "Destroying UCX listener";
    ucp_listener_destroy(listener_);
    listener_ = nullptr;
  }
}

void UcxManager::SendReadResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                  const std::optional<absl::Cord>& value) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send read response: client endpoint is null";
    return;
  }
  
  // Determine response status and value size
  uint32_t status_code = value.has_value() ? 0 : 1;  // 0 = success, 1 = not found
  uint32_t value_size = value.has_value() ? value->size() : 0;
  
  // Create response message structure
  ReadResponse response;
  response.header.magic_number = MESSAGE_MAGIC_NUMBER;
  response.header.type = MessageType::READ_RESPONSE;
  response.header.key_length = 0;  // No key in response
  response.header.value_length = value_size;
  response.header.request_id = request_id;
  response.status_code = status_code;
  
  // Calculate checksum for value data if present
  if (value.has_value() && value_size > 0) {
    std::string value_str = std::string(*value);
    response.header.checksum = message_utils::CalculateChecksum(value_str.data(), value_size);
  } else {
    response.header.checksum = 0;
  }
  
  // Debug: Log what we're about to send
  ABSL_LOG(INFO) << "Server sending read response: value_size=" << value_size 
                 << ", status_code=" << status_code;
  message_utils::LogMessageBuffer(reinterpret_cast<const char*>(&response), sizeof(ReadResponse), "Server sending read response header");
  
  if (value.has_value() && value_size > 0) {
    // Send header and value as separate UCX operations in sequence
    // First, allocate a contiguous buffer for the complete message
    size_t total_size = sizeof(ReadResponse) + value_size;
    std::unique_ptr<char[]> send_buffer(new char[total_size]);
    
    // Copy header
    std::memcpy(send_buffer.get(), &response, sizeof(ReadResponse));
    
    // Copy value data
    std::string value_str = std::string(*value);
    std::memcpy(send_buffer.get() + sizeof(ReadResponse), value_str.data(), value_size);
    
    // Debug hex dump
    std::stringstream hex_dump;
    for (size_t i = 0; i < std::min(total_size, static_cast<size_t>(64)); ++i) {
      hex_dump << std::hex << std::setfill('0') << std::setw(2) 
               << static_cast<unsigned char>(send_buffer[i]) << " ";
      if ((i + 1) % 16 == 0) hex_dump << "\n";
    }
    ABSL_LOG(INFO) << "Server hex dump of complete send buffer:\n" << hex_dump.str();
    
    // Send complete message using UCX
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = [](void* request, ucs_status_t status, void* user_data) {
      ABSL_LOG(INFO) << "Server read response sent with status: " << ucs_status_string(status);
      // Clean up the buffer
      std::unique_ptr<char[]>* buffer_ptr = static_cast<std::unique_ptr<char[]>*>(user_data);
      delete buffer_ptr;
      ucp_request_free(request);
    };
    
    // Transfer ownership of buffer to callback
    auto* buffer_ptr = new std::unique_ptr<char[]>(std::move(send_buffer));
    send_params.user_data = buffer_ptr;
    
    ucp_tag_t tag = UCX_TAG_READ_RESPONSE;
    void* request = ucp_tag_send_nbx(client_endpoint, buffer_ptr->get(), 
                                     total_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to send read response: " 
                      << ucs_status_string(UCS_PTR_STATUS(request));
      delete buffer_ptr;  // Clean up on error
    } else if (request == nullptr) {
      ABSL_LOG(INFO) << "Read response sent immediately";
      delete buffer_ptr;  // Clean up for immediate send
    } else {
      ABSL_LOG(INFO) << "Read response send in progress";
    }
  } else {
    // Send just the header for empty/not found responses
    ucp_request_param_t send_params;
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
    send_params.cb.send = [](void* request, ucs_status_t status, void* user_data) {
      ABSL_LOG(INFO) << "Server read response (header only) sent with status: " << ucs_status_string(status);
      ucp_request_free(request);
    };
    
    ucp_tag_t tag = UCX_TAG_READ_RESPONSE;
    void* request = ucp_tag_send_nbx(client_endpoint, &response, 
                                     sizeof(ReadResponse), tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to send read response: " 
                      << ucs_status_string(UCS_PTR_STATUS(request));
    } else if (request == nullptr) {
      ABSL_LOG(INFO) << "Read response sent immediately";
    } else {
      ABSL_LOG(INFO) << "Read response send in progress";
    }
  }
}

void UcxManager::SendWriteResponse(ucp_ep_h client_endpoint, uint64_t request_id, 
                                   uint32_t status_code) {
  if (!client_endpoint) {
    ABSL_LOG(ERROR) << "Cannot send write response: client endpoint is null";
    return;
  }
  
  // Create response message
  WriteResponse response;
  response.header.type = MessageType::WRITE_RESPONSE;
  response.header.key_length = 0;  // No key in response
  response.header.value_length = 0;  // No value in response
  response.header.request_id = request_id;
  response.status_code = status_code;
  
  // Send response using UCX
  ucp_request_param_t send_params;
  send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
  send_params.cb.send = [](void* request, ucs_status_t status, void* user_data) {
    ABSL_LOG(INFO) << "Server write response sent with status: " << ucs_status_string(status);
    ucp_request_free(request);
  };
  
  ucp_tag_t tag = UCX_TAG_WRITE_RESPONSE;  // Use distinct tag for write responses
  
  void* request = ucp_tag_send_nbx(client_endpoint, &response, sizeof(response), 
                                   tag, &send_params);
  
  if (UCS_PTR_IS_ERR(request)) {
    ABSL_LOG(ERROR) << "Failed to send write response: " 
                    << ucs_status_string(UCS_PTR_STATUS(request));
  } else if (request == nullptr) {
    ABSL_LOG(INFO) << "Write response sent immediately";
  } else {
    ABSL_LOG(INFO) << "Write response send in progress";
  }
}

void UcxManager::StartWorkerProgressTask() {
  if (progress_task_running_) {
    return;
  }
  
  progress_task_running_ = true;
  
  // Start a background thread for worker progress polling
  // In a real implementation, this should use TensorStore's thread pool
  std::thread([this]() {
    WorkerProgressTask();
  }).detach();
  
  ABSL_LOG(INFO) << "UCX worker progress task started";
}

void UcxManager::WorkerProgressTask() {
  ABSL_LOG(INFO) << "UCX worker progress polling started";
  
  while (true) {
    {
      absl::MutexLock lock(&mutex_);
      if (!initialized_ || !progress_task_running_) {
        break;
      }
      
      // Poll for progress
      ucp_worker_progress(worker_);
    }
    
    // Sleep briefly to avoid busy waiting
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  
  ABSL_LOG(INFO) << "UCX worker progress polling stopped";
}

void UcxManager::Shutdown() {
  absl::MutexLock lock(&mutex_);
  
  if (!initialized_) {
    return;
  }
  
  ABSL_LOG(INFO) << "Starting UCX Manager shutdown";
  
  // Stop the progress task first
  progress_task_running_ = false;
  
  // Cancel all pending receive operations
  CancelPendingReceivesNoLock();
  
  // Clean up the UCX listener
  CleanupListenerNoLock();
  
  // Clean up client endpoints
  for (ucp_ep_h client_endpoint : client_endpoints_) {
    if (client_endpoint) {
      ABSL_LOG(INFO) << "Destroying client endpoint";
      ucp_ep_destroy(client_endpoint);
    }
  }
  client_endpoints_.clear();
  
  // Clean up client-side endpoints
  for (ucp_ep_h client_side_endpoint : client_side_endpoints_) {
    if (client_side_endpoint) {
      ABSL_LOG(INFO) << "Destroying client-side endpoint";
      ucp_ep_destroy(client_side_endpoint);
    }
  }
  client_side_endpoints_.clear();
  
  // Complete any pending operations with cancelled status
  for (auto& [id, op] : pending_write_operations_) {
    op->promise.SetResult(absl::CancelledError("UCX Manager shutting down"));
  }
  pending_write_operations_.clear();
  
  for (auto& [id, op] : pending_read_operations_) {
    kvstore::ReadResult cancelled_result;
    cancelled_result.state = kvstore::ReadResult::kMissing;
    cancelled_result.stamp.generation = StorageGeneration::NoValue();
    cancelled_result.stamp.time = absl::Now();
    op->promise.SetResult(std::move(cancelled_result));
  }
  pending_read_operations_.clear();
  
  // Give UCX some time to process cancellations
  if (worker_) {
    // Process any remaining UCX events
    for (int i = 0; i < 10; ++i) {
      ucp_worker_progress(worker_);
    }
  }
  
  // Clean up UCX resources in reverse order of creation
  if (worker_) {
    ABSL_LOG(INFO) << "Destroying UCX worker";
    ucp_worker_destroy(worker_);
    worker_ = nullptr;
  }
  
  if (context_) {
    ABSL_LOG(INFO) << "Cleaning up UCX context";
    ucp_cleanup(context_);
    context_ = nullptr;
  }
  
  initialized_ = false;
  ABSL_LOG(INFO) << "UCX Manager shutdown completed";
}

UcxManager::~UcxManager() {
  Shutdown();
}

namespace {

class RemoteDramDriverSpec
    : public internal_kvstore::RegisteredDriverSpec<RemoteDramDriverSpec,
                                                    RemoteDramDriverSpecData> {
 public:
  /// Specifies the string identifier under which the driver will be registered.
  static constexpr char id[] = "remote_dram";

  Future<kvstore::DriverPtr> DoOpen() const override;

  Result<std::string> ToUrl(std::string_view path) const override {
    return absl::UnimplementedError(
        "remote_dram driver does not support URL conversion");
  }
};

/// Defines the "remote_dram" KeyValueStore driver.
class RemoteDramDriver
    : public internal_kvstore::RegisteredDriver<RemoteDramDriver,
                                                RemoteDramDriverSpec> {
 public:
  Future<kvstore::ReadResult> Read(kvstore::Key key, 
                                   kvstore::ReadOptions options) override {
    if (is_server_mode_) {
      // Server mode: read locally
      return ReadLocal(key, options);
    } else {
      // Client mode: send read request to remote server
      return ReadRemote(key, options);
    }
  }

  Future<TimestampedStorageGeneration> Write(kvstore::Key key,
                                             std::optional<absl::Cord> value,
                                             kvstore::WriteOptions options) override {
    if (!value.has_value()) {
      return absl::InvalidArgumentError("Write value cannot be null");
    }
    
    if (is_server_mode_) {
      // Server mode: write locally
      return WriteLocal(key, *value);
    } else {
      // Client mode: send to remote server
      return WriteRemote(key, *value);
    }
  }

  Future<const void> DeleteRange(KeyRange range) override {
    return absl::UnimplementedError("remote_dram driver DeleteRange not yet implemented");
  }

  void ListImpl(kvstore::ListOptions options, 
                kvstore::ListReceiver receiver) override {
    execution::set_error(receiver, 
                         absl::UnimplementedError("remote_dram driver List not yet implemented"));
  }

  absl::Status GetBoundSpecData(SpecData& spec) const {
    spec = spec_;
    return absl::OkStatus();
  }
  
  // Public members for access from driver spec
  SpecData spec_;
  ucp_ep_h client_endpoint_ = nullptr;  // UCX endpoint for client mode
  bool is_server_mode_ = false;
  
 private:
  Future<TimestampedStorageGeneration> WriteLocal(const kvstore::Key& key, 
                                                  const absl::Cord& value) {
    auto& storage = UcxManager::Instance().GetStorage();
    storage.Store(key, value);
    
    // Return success with current timestamp
    TimestampedStorageGeneration result;
    result.generation = StorageGeneration::FromString(
        absl::StrFormat("%d", std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    result.time = absl::Now();
    
    return MakeReadyFuture<TimestampedStorageGeneration>(result);
  }
  
  Future<TimestampedStorageGeneration> WriteRemote(const kvstore::Key& key, 
                                                   const absl::Cord& value) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "WriteRemote called but client_endpoint is null for key '" << key << "'";
      return absl::InternalError("Client endpoint not available");
    }
    
    // Use UCX networking for all connections
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    ABSL_LOG(INFO) << "WriteRemote sending data for key '" << key << "' with " << value.size() << " bytes to server";
    
    // Create write message
    size_t message_size = sizeof(MessageHeader) + key.size() + value.size();
    std::vector<char> message_buffer(message_size);
    
    // Copy key and value data first
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    // Handle absl::Cord properly
    std::string value_str = std::string(value);
    std::memcpy(data_ptr + key.size(), value_str.data(), value_str.size());
    
    // Initialize message header with integrity verification
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    message_utils::InitializeMessageHeader(header, MessageType::WRITE_REQUEST, 
                                          static_cast<uint32_t>(key.size()),
                                          static_cast<uint32_t>(value.size()),
                                          request_id, data_ptr);
    
    // Verify header was initialized correctly
    ABSL_LOG(INFO) << "Client initialized header: magic=" << std::dec << header->magic_number
                   << ", type=" << static_cast<uint32_t>(header->type)
                   << ", key_len=" << header->key_length
                   << ", value_len=" << header->value_length
                   << ", request_id=" << header->request_id
                   << ", checksum=" << header->checksum;
    
    // Log message for debugging
    message_utils::LogMessageBuffer(message_buffer.data(), message_size, "Client sending write request");
    
    ABSL_LOG(INFO) << "Sending UCX write request: key='" << key << "', value='" << value_str << "', request_id=" << request_id;
    std::cout << "📤 CLIENT: Sending write request [ID:" << request_id << "] key='" << key << "' value='" << value_str << "'" << std::endl;
    
    // Create promise/future pair
    auto [promise, future] = PromiseFuturePair<void>::Make();
    
    // Register pending operation
    ucx_manager.RegisterPendingOperation(request_id, std::move(promise), MessageType::WRITE_REQUEST);
    
    // Post a receive buffer to get the write response
    PostWriteResponseReceive(request_id);
    
    // Send message using UCX tagged messaging
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));  // Initialize all fields to zero
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    send_params.user_data = reinterpret_cast<void*>(request_id);
    
    ucp_tag_t tag = UCX_TAG_WRITE_REQUEST;  // Use distinct tag for write operations
    
    // Allocate a stable buffer that will survive until the send completes
    char* stable_buffer = new char[message_size];
    std::memcpy(stable_buffer, message_buffer.data(), message_size);
    
    ABSL_LOG(INFO) << "About to send: buffer_ptr=" << static_cast<void*>(stable_buffer)
                   << ", size=" << message_size << ", tag=" << std::dec << tag;
    
    // Double-check the buffer contents before sending
    message_utils::LogMessageBuffer(stable_buffer, message_size, "Final buffer before UCX send");
    
    // Store the buffer pointer in user_data so we can free it in the callback
    struct SendUserData {
      uint64_t request_id;
      char* buffer_to_free;
    };
    SendUserData* user_data = new SendUserData{request_id, stable_buffer};
    send_params.user_data = user_data;
    
    void* request = ucp_tag_send_nbx(client_endpoint_, stable_buffer, 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX send failed immediately: " << ucs_status_string(error_status);
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError(absl::StrFormat("UCX send failed: %s", 
              ucs_status_string(error_status))));
      // Clean up resources
      delete[] stable_buffer;
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately
      ABSL_LOG(INFO) << "UCX write request sent immediately for request_id=" << request_id;
      ucx_manager.CompletePendingOperation(request_id, absl::OkStatus());
      // Clean up resources
      delete[] stable_buffer;
      delete user_data;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      ABSL_LOG(INFO) << "UCX write request in progress for request_id=" << request_id;
      // Resources will be cleaned up in SendCallback
    }
    
    // Transform the void future to TimestampedStorageGeneration future
    auto [result_promise, result_future] = PromiseFuturePair<TimestampedStorageGeneration>::Make();
    
    // Chain the futures: when the void future completes, complete the result future
    std::move(future).ExecuteWhenReady([promise = std::move(result_promise)](ReadyFuture<void> ready_future) mutable {
      if (ready_future.status().ok()) {
        TimestampedStorageGeneration result;
        result.generation = StorageGeneration::FromString("remote_write");
        result.time = absl::Now();
        promise.SetResult(std::move(result));
      } else {
        promise.SetResult(ready_future.status());
      }
    });
    
    return result_future;
  }
  
  Future<kvstore::ReadResult> ReadLocal(const kvstore::Key& key, 
                                        const kvstore::ReadOptions& options) {
    auto& storage = UcxManager::Instance().GetStorage();
    auto value = storage.Get(key);
    
    if (!value.has_value()) {
      // Key not found
      kvstore::ReadResult result;
      result.state = kvstore::ReadResult::kMissing;
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // Key found, return the value
    kvstore::ReadResult result;
    result.state = kvstore::ReadResult::kValue;
    result.value = *value;
    result.stamp.generation = StorageGeneration::FromString(
        absl::StrFormat("%d", std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    result.stamp.time = absl::Now();
    
    return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
  }
  
  Future<kvstore::ReadResult> ReadRemote(const kvstore::Key& key, 
                                         const kvstore::ReadOptions& options) {
    if (!client_endpoint_) {
      ABSL_LOG(ERROR) << "ReadRemote called but client_endpoint is null for key '" << key << "'";
      
      kvstore::ReadResult result;
      result.state = kvstore::ReadResult::kMissing;
      result.stamp.generation = StorageGeneration::NoValue();
      result.stamp.time = absl::Now();
      return MakeReadyFuture<kvstore::ReadResult>(std::move(result));
    }
    
    // Use UCX networking for all connections
    auto& ucx_manager = UcxManager::Instance();
    uint64_t request_id = ucx_manager.GenerateRequestId();
    
    ABSL_LOG(INFO) << "ReadRemote sending read request for key '" << key << "' to server";
    
    // Create read request message
    size_t message_size = sizeof(MessageHeader) + key.size();
    std::vector<char> message_buffer(message_size);
    
    // Copy key data first
    char* data_ptr = message_buffer.data() + sizeof(MessageHeader);
    std::memcpy(data_ptr, key.data(), key.size());
    
    // Initialize message header with integrity verification
    MessageHeader* header = reinterpret_cast<MessageHeader*>(message_buffer.data());
    message_utils::InitializeMessageHeader(header, MessageType::READ_REQUEST, 
                                          static_cast<uint32_t>(key.size()),
                                          0,  // No value in read request
                                          request_id, data_ptr);
    
    // Log message buffer for debugging
    message_utils::LogMessageBuffer(message_buffer.data(), message_size, "Client sending read request");
    
    ABSL_LOG(INFO) << "Sending UCX read request: key='" << key << "', request_id=" << request_id;
    std::cout << "📤 CLIENT: Sending read request [ID:" << request_id << "] key='" << key << "'" << std::endl;
    
    // Create promise/future pair for read result
    auto [promise, future] = PromiseFuturePair<kvstore::ReadResult>::Make();
    
    // Register pending read operation
    ucx_manager.RegisterPendingReadOperation(request_id, std::move(promise));
    
    // Post a receive buffer to get the response
    PostReadResponseReceive(request_id);
    
    // Send read request using UCX tagged messaging
    ucp_request_param_t send_params;
    memset(&send_params, 0, sizeof(send_params));  // Initialize all fields to zero
    send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    send_params.cb.send = SendCallback;
    
    // Allocate a stable buffer that will survive until the send completes
    char* stable_buffer = new char[message_size];
    std::memcpy(stable_buffer, message_buffer.data(), message_size);
    
    // Store the buffer pointer in user_data so we can free it in the callback
    struct SendUserData {
      uint64_t request_id;
      char* buffer_to_free;
    };
    SendUserData* user_data = new SendUserData{request_id, stable_buffer};
    send_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_READ_REQUEST;  // Use distinct tag for read operations
    
    void* request = ucp_tag_send_nbx(client_endpoint_, stable_buffer, 
                                     message_size, tag, &send_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ucs_status_t error_status = UCS_PTR_STATUS(request);
      ABSL_LOG(ERROR) << "UCX read request send failed: " << ucs_status_string(error_status);
      
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
      // Clean up resources
      delete[] stable_buffer;
      delete user_data;
    } else if (request == nullptr) {
      // Send completed immediately - response handling will complete the read
      ABSL_LOG(INFO) << "UCX read request sent immediately for request_id=" << request_id;
      // Clean up resources
      delete[] stable_buffer;
      delete user_data;
    } else {
      // Send is in progress, will complete asynchronously via SendCallback
      ABSL_LOG(INFO) << "UCX read request in progress for request_id=" << request_id;
      // Resources will be cleaned up in SendCallback
    }
    
    return future;
  }
  
  /// Post a receive buffer to get read response from server
  void PostReadResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the read response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 64 * 1024;  // 64KB max response
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold both request_id and buffer
    struct ReadUserData {
      uint64_t request_id;
      char* buffer;
    };
    ReadUserData* user_data = new ReadUserData{request_id, recv_buffer};
    
    // Post non-blocking receive for read response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientReceiveCallback;
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_READ_RESPONSE;  // Use distinct tag for read responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      kvstore::ReadResult error_result;
      error_result.state = kvstore::ReadResult::kMissing;
      error_result.stamp.generation = StorageGeneration::NoValue();
      error_result.stamp.time = absl::Now();
      ucx_manager.CompletePendingReadOperation(request_id, std::move(error_result));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      ABSL_LOG(INFO) << "Posted client receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for read responses)
      ABSL_LOG(INFO) << "Client receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
      delete user_data;
    }
  }
  
  /// Post a receive buffer to get write response from server
  void PostWriteResponseReceive(uint64_t request_id) {
    // Post a receive buffer to get the write response from server
    auto& ucx_manager = UcxManager::Instance();
    
    // Allocate buffer for response
    constexpr size_t max_response_size = 1024;  // Write response is small
    char* recv_buffer = new char[max_response_size];
    
    // Create user data structure to hold both request_id and buffer (same pattern as read)
    struct WriteUserData {
      uint64_t request_id;
      char* buffer;
    };
    WriteUserData* user_data = new WriteUserData{request_id, recv_buffer};
    
    // Post non-blocking receive for write response
    ucp_request_param_t recv_params;
    recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    recv_params.cb.recv = ClientWriteResponseCallback;
    recv_params.user_data = user_data;
    
    ucp_tag_t tag = UCX_TAG_WRITE_RESPONSE;  // Use distinct tag for write responses
    ucp_tag_t tag_mask = UCX_TAG_MASK;  // Match tag group
    
    void* request = ucp_tag_recv_nbx(ucx_manager.GetWorker(), recv_buffer, max_response_size, 
                                     tag, tag_mask, &recv_params);
    
    if (UCS_PTR_IS_ERR(request)) {
      ABSL_LOG(ERROR) << "Failed to post client write response receive for request_id=" << request_id 
                      << ": " << ucs_status_string(UCS_PTR_STATUS(request));
      delete[] recv_buffer;
      delete user_data;
      
      // Complete the operation with error
      ucx_manager.CompletePendingOperation(request_id, 
          absl::InternalError("Failed to post write response receive"));
    } else if (request != nullptr) {
      // Request is in progress, will complete asynchronously
      ABSL_LOG(INFO) << "Posted client write response receive buffer for request_id=" << request_id;
    } else {
      // Receive completed immediately (unlikely for write responses)
      ABSL_LOG(INFO) << "Client write response receive completed immediately for request_id=" << request_id;
      delete[] recv_buffer;
      delete user_data;
    }
  }
};

Future<kvstore::DriverPtr> RemoteDramDriverSpec::DoOpen() const {
  // Validate that either listen_addr or remote_addr is specified, but not both
  if (data_.listen_addr.has_value() && data_.remote_addr.has_value()) {
    return absl::InvalidArgumentError(
        "Cannot specify both listen_addr and remote_addr");
  }
  
  if (!data_.listen_addr.has_value() && !data_.remote_addr.has_value()) {
    return absl::InvalidArgumentError(
        "Must specify either listen_addr (server mode) or remote_addr (client mode)");
  }

  auto driver = internal::MakeIntrusivePtr<RemoteDramDriver>();
  driver->spec_ = data_;
  
  auto& ucx_manager = UcxManager::Instance();
  auto init_status = ucx_manager.Initialize();
  if (!init_status.ok()) {
    return init_status;
  }
  
  // Initialize UCX for server or client mode
  if (data_.listen_addr.has_value()) {
    // Server mode
    ABSL_LOG(INFO) << "Initializing UCX for server mode on " << *data_.listen_addr;
    
    // Create UCX listener
    auto listener_result = ucx_manager.CreateListener(*data_.listen_addr);
    if (!listener_result.ok()) {
      return listener_result.status();
    }
    
    driver->is_server_mode_ = true;
    ABSL_LOG(INFO) << "UCX server initialized successfully, listening on " << *data_.listen_addr;
  } else {
    // Client mode - create UCX endpoint to server
    ABSL_LOG(INFO) << "Initializing UCX for client mode to " << *data_.remote_addr;
    
    // Create UCX endpoint to server
    auto endpoint_result = ucx_manager.CreateClientEndpoint(*data_.remote_addr);
    if (!endpoint_result.ok()) {
      return endpoint_result.status();
    }
    
    driver->client_endpoint_ = *endpoint_result;
    driver->is_server_mode_ = false;
    ABSL_LOG(INFO) << "UCX client initialized successfully, connected to " << *data_.remote_addr;
  }
  
  return kvstore::DriverPtr(driver.get());
}

Result<kvstore::Spec> ParseRemoteDramUrl(std::string_view url) {
  return absl::UnimplementedError(
      "remote_dram driver URL parsing not yet implemented");
}

}  // namespace
}  // namespace tensorstore

TENSORSTORE_DECLARE_GARBAGE_COLLECTION_NOT_REQUIRED(
    tensorstore::RemoteDramDriver)

// Registers the driver.
namespace {
const tensorstore::internal_kvstore::DriverRegistration<
    tensorstore::RemoteDramDriverSpec>
    registration;

const tensorstore::internal_kvstore::UrlSchemeRegistration
    url_scheme_registration{tensorstore::RemoteDramDriverSpec::id,
                            tensorstore::ParseRemoteDramUrl};
}  // namespace 