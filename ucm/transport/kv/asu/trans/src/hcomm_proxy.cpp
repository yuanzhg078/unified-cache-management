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
#include "hcomm_proxy.h"

extern "C" {
__attribute__((weak)) HcommResult HcommEndpointCreate(const EndpointDesc* endpoint,
                                                      EndpointHandle* endpoint_handle);
__attribute__((weak)) HcommResult HcommEndpointDestroy(EndpointHandle endpoint_handle);
__attribute__((weak)) HcommResult HcommMemReg(EndpointHandle endpoint_handle, const char* mem_tag,
                                              const CommMem* mem, HcommMemHandle* mem_handle);
__attribute__((weak)) HcommResult HcommMemUnreg(EndpointHandle endpoint_handle,
                                                HcommMemHandle mem_handle);
__attribute__((weak)) HcommResult HcommMemExport(EndpointHandle endpoint_handle,
                                                 HcommMemHandle mem_handle, void** memDesc,
                                                 std::uint32_t* mem_desc_len);
__attribute__((weak)) HcommResult HcommMemImport(EndpointHandle endpoint_handle,
                                                 const void* memDesc, std::uint32_t desc_len,
                                                 CommMem* out_mem);
__attribute__((weak)) HcommResult HcommMemUnimport(EndpointHandle endpoint_handle,
                                                   const void* memDesc, std::uint32_t desc_len);
__attribute__((weak)) HcommResult HcommChannelCreate(EndpointHandle endpoint_handle, CommEngine engine,
                                                     HcommChannelDesc* channel_descs,
                                                     std::uint32_t channel_num,
                                                     ChannelHandle* channels);
__attribute__((weak)) HcommResult HcommChannelGetStatus(const ChannelHandle* channel_list,
                                                        std::uint32_t list_num,
                                                        std::int32_t* statusList);
__attribute__((weak)) HcommResult HcommChannelDestroy(const ChannelHandle* channels,
                                                      std::uint32_t channel_num);
__attribute__((weak)) HcommResult HcommThreadAlloc(CommEngine engine, std::uint32_t thread_num,
                                                   const std::uint32_t* notify_num_per_thread,
                                                   ThreadHandle* threads);
__attribute__((weak)) HcommResult HcommThreadFree(const ThreadHandle* threads,
                                                  std::uint32_t thread_num);
__attribute__((weak)) std::int32_t HcommWriteNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                                         void* dst, const void* src,
                                                         std::uint64_t len);
__attribute__((weak)) std::int32_t HcommReadNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                                        void* dst, const void* src,
                                                        std::uint64_t len);
__attribute__((weak)) std::int32_t HcommReadOnThread(ThreadHandle thread, ChannelHandle channel,
                                                     void* dst, const void* src,
                                                     std::uint64_t len);
__attribute__((weak)) std::int32_t HcommWriteOnThread(ThreadHandle thread, ChannelHandle channel,
                                                      void* dst, const void* src,
                                                      std::uint64_t len);
__attribute__((weak)) std::int32_t HcommChannelFenceOnThread(ThreadHandle thread,
                                                             ChannelHandle channel);
__attribute__((weak)) std::int32_t HcommBatchModeStart(const char* batch_tag);
__attribute__((weak)) std::int32_t HcommBatchModeEnd(const char* batch_tag);
}

namespace UC::ASU {
namespace {

constexpr HcommResult kHcommNotSupport = 9;

}  // namespace

HcommResult HcommProxy::EndpointCreate(const EndpointDesc* endpoint,
                                       EndpointHandle* endpoint_handle)
{
    if (HcommEndpointCreate == nullptr) { return kHcommNotSupport; }
    return HcommEndpointCreate(endpoint, endpoint_handle);
}

HcommResult HcommProxy::EndpointDestroy(EndpointHandle endpoint_handle)
{
    if (HcommEndpointDestroy == nullptr) { return kHcommNotSupport; }
    return HcommEndpointDestroy(endpoint_handle);
}

HcommResult HcommProxy::MemReg(EndpointHandle endpoint_handle, const char* mem_tag,
                               const CommMem* mem, HcommMemHandle* mem_handle)
{
    if (HcommMemReg == nullptr) { return kHcommNotSupport; }
    return HcommMemReg(endpoint_handle, mem_tag, mem, mem_handle);
}

HcommResult HcommProxy::MemUnreg(EndpointHandle endpoint_handle, HcommMemHandle mem_handle)
{
    if (HcommMemUnreg == nullptr) { return kHcommNotSupport; }
    return HcommMemUnreg(endpoint_handle, mem_handle);
}

HcommResult HcommProxy::MemExport(EndpointHandle endpoint_handle, HcommMemHandle mem_handle,
                                  void** mem_desc, std::uint32_t* mem_desc_len)
{
    if (HcommMemExport == nullptr) { return kHcommNotSupport; }
    return HcommMemExport(endpoint_handle, mem_handle, mem_desc, mem_desc_len);
}

HcommResult HcommProxy::MemImport(EndpointHandle endpoint_handle, const void* mem_desc,
                                  std::uint32_t desc_len, CommMem* out_mem)
{
    if (HcommMemImport == nullptr) { return kHcommNotSupport; }
    return HcommMemImport(endpoint_handle, mem_desc, desc_len, out_mem);
}

HcommResult HcommProxy::MemUnimport(EndpointHandle endpoint_handle, const void* mem_desc,
                                    std::uint32_t desc_len)
{
    if (HcommMemUnimport == nullptr) { return kHcommNotSupport; }
    return HcommMemUnimport(endpoint_handle, mem_desc, desc_len);
}

HcommResult HcommProxy::ChannelCreate(EndpointHandle endpoint_handle, CommEngine engine,
                                      HcommChannelDesc* channel_descs,
                                      std::uint32_t channel_num, ChannelHandle* channels)
{
    if (HcommChannelCreate == nullptr) { return kHcommNotSupport; }
    return HcommChannelCreate(endpoint_handle, engine, channel_descs, channel_num, channels);
}

HcommResult HcommProxy::ChannelDestroy(const ChannelHandle* channels, std::uint32_t channel_num)
{
    if (HcommChannelDestroy == nullptr) { return kHcommNotSupport; }
    return HcommChannelDestroy(channels, channel_num);
}

HcommResult HcommProxy::ChannelGetStatus(const ChannelHandle* channel_list,
                                         std::uint32_t list_num, std::int32_t* status_list)
{
    if (HcommChannelGetStatus == nullptr) { return kHcommNotSupport; }
    return HcommChannelGetStatus(channel_list, list_num, status_list);
}

HcommResult HcommProxy::ThreadAlloc(CommEngine engine, std::uint32_t thread_num,
                                    const std::uint32_t* notify_num_per_thread,
                                    ThreadHandle* threads)
{
    if (HcommThreadAlloc == nullptr) { return kHcommNotSupport; }
    return HcommThreadAlloc(engine, thread_num, notify_num_per_thread, threads);
}

HcommResult HcommProxy::ThreadFree(const ThreadHandle* threads, std::uint32_t thread_num)
{
    if (HcommThreadFree == nullptr) { return kHcommNotSupport; }
    return HcommThreadFree(threads, thread_num);
}

std::int32_t HcommProxy::WriteNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                          void* dst, const void* src, std::uint64_t len)
{
    if (HcommWriteNbiOnThread == nullptr) { return kHcommNotSupport; }
    return HcommWriteNbiOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommProxy::ReadNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                         void* dst, const void* src, std::uint64_t len)
{
    if (HcommReadNbiOnThread == nullptr) { return kHcommNotSupport; }
    return HcommReadNbiOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommProxy::ReadOnThread(ThreadHandle thread, ChannelHandle channel,
                                      void* dst, const void* src, std::uint64_t len)
{
    if (HcommReadOnThread == nullptr) { return kHcommNotSupport; }
    return HcommReadOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommProxy::WriteOnThread(ThreadHandle thread, ChannelHandle channel,
                                       void* dst, const void* src, std::uint64_t len)
{
    if (HcommWriteOnThread == nullptr) { return kHcommNotSupport; }
    return HcommWriteOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommProxy::ChannelFenceOnThread(ThreadHandle thread, ChannelHandle channel)
{
    if (HcommChannelFenceOnThread == nullptr) { return kHcommNotSupport; }
    return HcommChannelFenceOnThread(thread, channel);
}

std::int32_t HcommProxy::BatchModeStart(const char* batch_tag)
{
    if (HcommBatchModeStart == nullptr) { return kHcommNotSupport; }
    return HcommBatchModeStart(batch_tag);
}

std::int32_t HcommProxy::BatchModeEnd(const char* batch_tag)
{
    if (HcommBatchModeEnd == nullptr) { return kHcommNotSupport; }
    return HcommBatchModeEnd(batch_tag);
}

}  // namespace UC::ASU
