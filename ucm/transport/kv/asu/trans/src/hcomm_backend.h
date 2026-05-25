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

#include <memory>
#include <mutex>
#include <unordered_map>
#include "connection_backend.h"
#include "hcomm_endpoint.h"
#include "hcomm/hcomm_res_defs.h"

namespace UC::ASU {

class HcommBackend final : public ConnectionBackend {
public:
    HcommBackend() = default;
    ~HcommBackend() override;

    Status Initialize(const ConnectionManagerConfig& config) override;
    void Finalize() override;

    Status CreateConnection(const CreateConnectionRequest& request,
                            std::vector<ConnectionHandle>& connection_handles) override;

    std::vector<Status> DeleteConnections(
        const std::vector<ConnectionHandle>& connection_handles) override;

    std::vector<Status> Send(const std::vector<SendIoBatch>& io_batches,
                             std::uint32_t kernel_count,
                             std::uint32_t quiet_count) override;

    Status RegisterMemory(ConnectionHandle connection_handle,
                          const std::vector<RegisterMemoryDesc>& memory_descs,
                          std::vector<CommMemHandle>& memory_handles) override;

    std::vector<Status> UnregisterMemory(
        const std::vector<UnregisterMemoryDesc>& memory_descs) override;

private:
    struct HcommConnection {
        std::shared_ptr<HcommEndpoint> endpoint;
        ChannelHandle channel_handle{0};
        std::uint64_t send_size{0};
        std::uint64_t flag_size{1};
        void* remote_send_addr{nullptr};
        void* remote_flag_addr{nullptr};
        bool owns_channel{false};
    };

    Status BuildEndpoint(const std::string& ip, const std::string& attr_prefix,
                         EndpointDesc& endpoint) const;
    Status FillEndpointProtocol(const std::string& attr_prefix, EndpointDesc& endpoint) const;
    Status FillEndpointAddress(const std::string& ip, const std::string& attr_prefix,
                               EndpointDesc& endpoint) const;
    Status FillEndpointLocation(const std::string& attr_prefix, EndpointDesc& endpoint) const;
    Status LoadEndpointConfigFile();
    Status CreateConnectionsOnEndpoint(const std::string& local_ip, const std::string& remote_ip,
                                       std::uint32_t port, std::uint32_t qp_num,
                                       std::vector<ConnectionHandle>& connection_handles);
    Status DestroyOneConnection(ConnectionHandle handle);
    Status SendOne(const SendIoBatch& io_batch, std::uint32_t index);
    Status RegisterOne(HcommConnection& connection, const RegisterMemoryDesc& desc,
                       CommMemHandle& memory_handle);
    Status UnregisterOne(const UnregisterMemoryDesc& desc);

    std::uint64_t GetAttrU64(const std::string& key, std::uint64_t default_value) const;
    std::string GetAttrString(const std::string& key, const std::string& default_value) const;

    ConnectionManagerConfig config_;
    const std::unordered_map<std::string, std::string>* active_attrs_{nullptr};
    bool initialized_{false};
    std::uint8_t tc_{0};
    std::uint8_t sl_{0};
    std::uint64_t default_send_size_{0};
    std::uint64_t default_flag_size_{1};
    void* default_remote_send_addr_{nullptr};
    void* default_remote_flag_addr_{nullptr};
    std::unordered_map<ConnectionHandle, HcommConnection> connections_;
    std::mutex mu_;
};

}  // namespace UC::ASU
