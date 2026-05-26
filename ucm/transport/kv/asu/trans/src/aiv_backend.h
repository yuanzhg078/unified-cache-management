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

#include "connection_backend.h"

namespace UC::ASU {

class AivBackend final : public ConnectionBackend {
public:
    Status Initialize(const ConnectionManagerConfig& config) override;
    void Finalize() override;

    Status CreateConnection(const CreateConnectionRequest& request,
                            ConnectionEndpointHandle& endpoint_handle,
                            std::vector<ConnectionHandle>& connection_handles) override;

    std::vector<Status> DeleteConnections(
        const std::vector<ConnectionHandle>& connection_handles) override;

    std::vector<Status> Send(const std::vector<SendIoBatch>& io_batches,
                             std::uint32_t kernel_count,
                             std::uint32_t quiet_count) override;

    Status RegisterMemory(ConnectionEndpointHandle endpoint_handle,
                          const std::vector<RegisterMemoryDesc>& memory_descs,
                          std::vector<CommMemHandle>& memory_handles) override;

    std::vector<Status> UnregisterMemory(
        const std::vector<UnregisterMemoryDesc>& memory_descs) override;
};

}  // namespace UC::ASU
