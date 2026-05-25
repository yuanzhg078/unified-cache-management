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
#include "aiv_backend.h"

namespace UC::ASU {
namespace {

Status Unsupported()
{
    return Status::Error(StatusCode::UNSUPPORTED, "aiv backend is not implemented yet");
}

std::vector<Status> UnsupportedBatch(std::size_t count)
{
    std::vector<Status> statuses;
    statuses.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { statuses.emplace_back(Unsupported()); }
    return statuses;
}

}  // namespace

Status AivBackend::Initialize(const ConnectionManagerConfig& config)
{
    (void)config;
    return Status::OK();
}

void AivBackend::Finalize() {}

Status AivBackend::CreateConnection(const CreateConnectionRequest& request,
                                    std::vector<ConnectionHandle>& connection_handles)
{
    (void)request;
    connection_handles.clear();
    return Unsupported();
}

std::vector<Status> AivBackend::DeleteConnections(
    const std::vector<ConnectionHandle>& connection_handles)
{
    return UnsupportedBatch(connection_handles.size());
}

std::vector<Status> AivBackend::Send(const std::vector<SendIoBatch>& io_batches,
                                     std::uint32_t kernel_count,
                                     std::uint32_t quiet_count)
{
    (void)kernel_count;
    (void)quiet_count;
    return UnsupportedBatch(io_batches.size());
}

Status AivBackend::RegisterMemory(ConnectionHandle connection_handle,
                                  const std::vector<RegisterMemoryDesc>& memory_descs,
                                  std::vector<CommMemHandle>& memory_handles)
{
    (void)connection_handle;
    memory_handles.assign(memory_descs.size(), kInvalidCommMemHandle);
    return Unsupported();
}

std::vector<Status> AivBackend::UnregisterMemory(
    const std::vector<UnregisterMemoryDesc>& memory_descs)
{
    return UnsupportedBatch(memory_descs.size());
}

}  // namespace UC::ASU
