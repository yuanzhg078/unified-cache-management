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
#include "hcomm_endpoint.h"

#include <string>
#include <utility>
#include "hcomm_proxy.h"

namespace UC::ASU {
namespace {

constexpr std::uint32_t kDefaultNotifyNum = 1U;
constexpr HcommResult kHcommSuccess = 0;
constexpr HcommResult kHcommNotSupport = 9;

Status HcommStatus(HcommResult ret, std::string op)
{
    if (ret == kHcommSuccess) { return Status::OK(); }
    auto code = ret == kHcommNotSupport ? StatusCode::UNSUPPORTED : StatusCode::IO_ERROR;
    return Status::Error(code, std::move(op) + " failed, hcomm ret=" + std::to_string(ret));
}

Status NotFound(std::string message)
{
    return Status::Error(StatusCode::NOT_FOUND, std::move(message));
}

}  // namespace

HcommEndpoint::HcommEndpoint(EndpointDesc local_endpoint, EndpointDesc remote_endpoint)
    : local_endpoint_(local_endpoint), remote_endpoint_(remote_endpoint)
{
}

HcommEndpoint::~HcommEndpoint()
{
    (void)Finalize();
}

Status HcommEndpoint::Initialize()
{
    if (initialized_) { return Status::OK(); }

    auto ret = HcommProxy::EndpointCreate(&local_endpoint_, &endpoint_handle_);
    if (ret != kHcommSuccess) { return HcommStatus(ret, "HcommEndpointCreate"); }
    owns_endpoint_ = true;

    std::uint32_t notify_num = kDefaultNotifyNum;
    ret = HcommProxy::ThreadAlloc(GetCommEngine(), 1, &notify_num, &thread_handle_);
    if (ret != kHcommSuccess) {
        (void)HcommProxy::EndpointDestroy(endpoint_handle_);
        endpoint_handle_ = nullptr;
        owns_endpoint_ = false;
        return HcommStatus(ret, "HcommThreadAlloc");
    }
    owns_thread_ = true;
    initialized_ = true;
    return Status::OK();
}

Status HcommEndpoint::Finalize()
{
    Status first = Status::OK();
    for (auto channel_handle : channels_) {
        auto channel = channel_handle;
        auto ret = HcommProxy::ChannelDestroy(&channel, 1);
        if (ret != kHcommSuccess && first.ok()) {
            first = HcommStatus(ret, "HcommChannelDestroy");
        }
    }
    channels_.clear();

    for (auto memory_handle : memory_handles_) {
        auto ret = HcommProxy::MemUnreg(endpoint_handle_, memory_handle);
        if (ret != kHcommSuccess && first.ok()) {
            first = HcommStatus(ret, "HcommMemUnreg");
        }
    }
    memory_handles_.clear();

    if (owns_thread_ && thread_handle_ != 0) {
        auto ret = HcommProxy::ThreadFree(&thread_handle_, 1);
        if (ret != kHcommSuccess && first.ok()) {
            first = HcommStatus(ret, "HcommThreadFree");
        }
        thread_handle_ = 0;
        owns_thread_ = false;
    }
    if (owns_endpoint_ && endpoint_handle_ != nullptr) {
        auto ret = HcommProxy::EndpointDestroy(endpoint_handle_);
        if (ret != kHcommSuccess && first.ok()) {
            first = HcommStatus(ret, "HcommEndpointDestroy");
        }
        endpoint_handle_ = nullptr;
        owns_endpoint_ = false;
    }
    initialized_ = false;
    return first;
}

Status HcommEndpoint::CreateChannels(std::uint32_t port, std::uint32_t channel_num,
                                     std::uint8_t tc, std::uint8_t sl,
                                     std::uint32_t hccs_qos,
                                     std::vector<ChannelHandle>& channel_handles)
{
    if (!initialized_) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "hcomm endpoint is not initialized");
    }

    std::vector<HcommChannelDesc> channel_descs(channel_num);
    auto ret = HcommChannelDescInit(channel_descs.data(), channel_num);
    if (ret != kHcommSuccess) { return HcommStatus(ret, "HcommChannelDescInit"); }

    for (std::uint32_t i = 0; i < channel_num; ++i) {
        auto& channel_desc = channel_descs[i];
        channel_desc.role = HCOMM_SOCKET_ROLE_CLIENT;
        channel_desc.remoteEndpoint = remote_endpoint_;
        channel_desc.notifyNum = kDefaultNotifyNum;
        channel_desc.exchangeAllMems = true;
        channel_desc.port = static_cast<std::uint16_t>(port);
        if (local_endpoint_.protocol == COMM_PROTOCOL_ROCE) {
            channel_desc.roceAttr.queueNum = 1;
            channel_desc.roceAttr.tc = tc;
            channel_desc.roceAttr.sl = sl;
        } else if (local_endpoint_.protocol == COMM_PROTOCOL_HCCS) {
            channel_desc.hccsAttr.qos = hccs_qos;
        }
        *reinterpret_cast<std::uint32_t*>(
            channel_desc.raws + sizeof(channel_desc.raws) - sizeof(std::uint32_t)) = i;
    }

    channel_handles.assign(channel_num, 0);
    ret = HcommProxy::ChannelCreate(endpoint_handle_, GetCommEngine(), channel_descs.data(),
                                    channel_num, channel_handles.data());
    if (ret != kHcommSuccess) {
        for (auto channel_handle : channel_handles) {
            if (channel_handle != 0) {
                (void)HcommProxy::ChannelDestroy(&channel_handle, 1);
            }
        }
        channel_handles.clear();
        return HcommStatus(ret, "HcommChannelCreate");
    }

    for (auto channel_handle : channel_handles) {
        channels_.insert(channel_handle);
    }
    return Status::OK();
}

Status HcommEndpoint::DestroyChannel(ChannelHandle channel_handle)
{
    auto it = channels_.find(channel_handle);
    if (it == channels_.end()) { return NotFound("hcomm channel handle not found"); }
    auto channel = channel_handle;
    auto ret = HcommProxy::ChannelDestroy(&channel, 1);
    if (ret == kHcommSuccess) { channels_.erase(it); }
    return HcommStatus(ret, "HcommChannelDestroy");
}

Status HcommEndpoint::RegisterMemory(const CommMem& mem, CommMemHandle& memory_handle)
{
    HcommMemHandle hcomm_mem_handle = nullptr;
    auto ret = HcommProxy::MemReg(endpoint_handle_, nullptr, &mem, &hcomm_mem_handle);
    if (ret != kHcommSuccess) { return HcommStatus(ret, "HcommMemReg"); }
    auto insert_result = memory_handles_.insert(hcomm_mem_handle);
    if (!insert_result.second) {
        (void)HcommProxy::MemUnreg(endpoint_handle_, hcomm_mem_handle);
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             "duplicate hcomm memory handle returned");
    }
    memory_handle = hcomm_mem_handle;
    return Status::OK();
}

Status HcommEndpoint::UnregisterMemory(CommMemHandle memory_handle)
{
    auto it = memory_handles_.find(memory_handle);
    if (it == memory_handles_.end()) { return NotFound("hcomm memory handle not found"); }
    auto ret = HcommProxy::MemUnreg(endpoint_handle_, memory_handle);
    if (ret == kHcommSuccess) { memory_handles_.erase(it); }
    return HcommStatus(ret, "HcommMemUnreg");
}

ThreadHandle HcommEndpoint::GetThreadHandle() const
{
    return thread_handle_;
}

EndpointHandle HcommEndpoint::GetEndpointHandle() const
{
    return endpoint_handle_;
}

const EndpointDesc& HcommEndpoint::GetLocalEndpoint() const
{
    return local_endpoint_;
}

const EndpointDesc& HcommEndpoint::GetRemoteEndpoint() const
{
    return remote_endpoint_;
}

CommEngine HcommEndpoint::GetCommEngine() const
{
    if (local_endpoint_.loc.locType == ENDPOINT_LOC_TYPE_DEVICE) {
        return COMM_ENGINE_AICPU;
    }
    return COMM_ENGINE_CPU;
}

}  // namespace UC::ASU
