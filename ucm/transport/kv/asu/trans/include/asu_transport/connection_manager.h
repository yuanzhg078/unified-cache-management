/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "asu_transport/types.h"

namespace UC::ASU {

using ConnectionHandle = std::uint64_t;
using ConnectionEndpointHandle = std::uint64_t;
using CommMemHandle = void*;

constexpr ConnectionHandle kInvalidConnectionHandle = 0;
constexpr ConnectionEndpointHandle kInvalidConnectionEndpointHandle = 0;
constexpr CommMemHandle kInvalidCommMemHandle = nullptr;

enum class ConnectionBackendType {
    HCOMM = 0,
    AIV = 1,
};

enum class ConnectionMemType {
    DEVICE = 0,
    HOST = 1,
};

struct ConnectionManagerConfig {
    ConnectionBackendType backend_type{ConnectionBackendType::HCOMM};
    std::unordered_map<std::string, std::string> attrs;
};

struct CreateConnectionRequest {
    std::string local_ip;
    std::string remote_ip;
    std::uint32_t port{0};
    std::uint32_t qp_num{0};
    std::uint32_t timeout_ms{0};
    std::unordered_map<std::string, std::string> attrs;
};

struct SendIoBatch {
    ConnectionHandle connection_handle{kInvalidConnectionHandle};
    void* send_buffer{nullptr};
    void* flag_buffer{nullptr};
};

struct RegisterMemoryDesc {
    ConnectionMemType memory_type{ConnectionMemType::DEVICE};
    std::uintptr_t addr{0};
    std::size_t size{0};
};

struct UnregisterMemoryDesc {
    ConnectionEndpointHandle endpoint_handle{kInvalidConnectionEndpointHandle};
    CommMemHandle memory_handle{kInvalidCommMemHandle};
};

class ConnectionManager {
public:
    ConnectionManager();
    explicit ConnectionManager(ConnectionManagerConfig config);
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    ConnectionManager(ConnectionManager&&) noexcept;
    ConnectionManager& operator=(ConnectionManager&&) noexcept;

    Status Initialize();
    void Finalize();

    Status CreateConnection(const CreateConnectionRequest& request,
                            std::vector<ConnectionHandle>& connection_handles);
    Status CreateConnection(const CreateConnectionRequest& request,
                            ConnectionEndpointHandle& endpoint_handle,
                            std::vector<ConnectionHandle>& connection_handles);

    std::vector<Status> DeleteConnections(const std::vector<ConnectionHandle>& connection_handles);

    std::vector<Status> Send(const std::vector<SendIoBatch>& io_batches,
                             std::uint32_t kernel_count, std::uint32_t quiet_count);

    Status RegisterMemory(ConnectionEndpointHandle endpoint_handle,
                          const std::vector<RegisterMemoryDesc>& memory_descs,
                          std::vector<CommMemHandle>& memory_handles);

    std::vector<Status> UnregisterMemory(const std::vector<UnregisterMemoryDesc>& memory_descs);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<ConnectionManager> CreateConnectionManager(
    ConnectionManagerConfig config = {});

}  // namespace UC::ASU
