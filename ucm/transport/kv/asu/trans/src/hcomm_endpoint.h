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

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>
#include "asu_transport/connection_manager.h"
#include "hcomm/hcomm_res_defs.h"

namespace UC::ASU {

class HcommEndpoint {
public:
    HcommEndpoint(EndpointDesc local_endpoint, EndpointDesc remote_endpoint);
    ~HcommEndpoint();

    HcommEndpoint(const HcommEndpoint&) = delete;
    HcommEndpoint& operator=(const HcommEndpoint&) = delete;

    Status Initialize();
    Status Finalize();

    Status CreateChannels(std::uint32_t port, std::uint32_t channel_num,
                          std::uint8_t tc, std::uint8_t sl, std::uint32_t hccs_qos,
                          std::vector<ChannelHandle>& channel_handles);
    Status CreateChannel(std::uint32_t port, std::uint8_t tc, std::uint8_t sl,
                         std::uint32_t hccs_qos, std::uint32_t channel_index,
                         ChannelHandle& channel_handle);
    Status DestroyChannel(ChannelHandle channel_handle);

    Status RegisterMemory(const CommMem& mem, CommMemHandle& memory_handle);
    Status UnregisterMemory(CommMemHandle memory_handle);
    Status ImportMemory(const void* memory_desc, std::uint32_t desc_len, CommMem& memory);
    Status UnimportMemory(const void* memory_desc, std::uint32_t desc_len);

    ThreadHandle GetThreadHandle() const;
    EndpointHandle GetEndpointHandle() const;
    const EndpointDesc& GetLocalEndpoint() const;
    const EndpointDesc& GetRemoteEndpoint() const;

private:
    CommEngine GetCommEngine() const;

    EndpointDesc local_endpoint_{};
    EndpointDesc remote_endpoint_{};
    EndpointHandle endpoint_handle_{nullptr};
    ThreadHandle thread_handle_{0};
    bool initialized_{false};
    bool owns_endpoint_{false};
    bool owns_thread_{false};
    std::unordered_set<ChannelHandle> channels_;
    std::unordered_set<CommMemHandle> memory_handles_;
    std::mutex mu_;
};

}  // namespace UC::ASU
