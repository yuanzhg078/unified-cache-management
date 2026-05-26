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
#include "hcomm_stub.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include "hcomm/hcomm_res_defs.h"

namespace {

constexpr HcommResult kSuccess = 0;

struct StubState {
    std::atomic<std::int32_t> next_endpoint_create_result{kSuccess};
    std::atomic<std::int32_t> next_thread_alloc_result{kSuccess};
    std::atomic<std::int32_t> next_channel_create_result{kSuccess};
    std::atomic<std::int32_t> next_mem_import_result{kSuccess};
    std::atomic<std::int32_t> next_write_result{kSuccess};
    std::atomic<std::int32_t> next_fence_result{kSuccess};

    std::atomic<std::uint32_t> endpoint_create_count{0};
    std::atomic<std::uint32_t> endpoint_destroy_count{0};
    std::atomic<std::uint32_t> thread_alloc_count{0};
    std::atomic<std::uint32_t> thread_free_count{0};
    std::atomic<std::uint32_t> channel_create_count{0};
    std::atomic<std::uint32_t> channel_destroy_count{0};
    std::atomic<std::uint32_t> mem_reg_count{0};
    std::atomic<std::uint32_t> mem_unreg_count{0};
    std::atomic<std::uint32_t> mem_import_count{0};
    std::atomic<std::uint32_t> mem_unimport_count{0};
    std::atomic<std::uint32_t> write_count{0};
    std::atomic<std::uint32_t> fence_count{0};

    std::atomic<std::uintptr_t> next_endpoint_handle{1};
    std::atomic<std::uintptr_t> next_mem_handle{1};
    std::atomic<std::uint64_t> next_channel_handle{1};
    std::atomic<std::uint64_t> next_thread_handle{1};
};

StubState g_state;

std::int32_t Consume(std::atomic<std::int32_t>& result)
{
    return result.exchange(kSuccess);
}

void ResetCounters()
{
    g_state.endpoint_create_count = 0;
    g_state.endpoint_destroy_count = 0;
    g_state.thread_alloc_count = 0;
    g_state.thread_free_count = 0;
    g_state.channel_create_count = 0;
    g_state.channel_destroy_count = 0;
    g_state.mem_reg_count = 0;
    g_state.mem_unreg_count = 0;
    g_state.mem_import_count = 0;
    g_state.mem_unimport_count = 0;
    g_state.write_count = 0;
    g_state.fence_count = 0;
}

void ResetHandles()
{
    g_state.next_endpoint_handle = 1;
    g_state.next_mem_handle = 1;
    g_state.next_channel_handle = 1;
    g_state.next_thread_handle = 1;
}

}  // namespace

extern "C" {

void UcmHcommStubReset()
{
    g_state.next_endpoint_create_result = kSuccess;
    g_state.next_thread_alloc_result = kSuccess;
    g_state.next_channel_create_result = kSuccess;
    g_state.next_mem_import_result = kSuccess;
    g_state.next_write_result = kSuccess;
    g_state.next_fence_result = kSuccess;
    ResetCounters();
    ResetHandles();
}

void UcmHcommStubSetNextEndpointCreateResult(std::int32_t result)
{
    g_state.next_endpoint_create_result = result;
}

void UcmHcommStubSetNextThreadAllocResult(std::int32_t result)
{
    g_state.next_thread_alloc_result = result;
}

void UcmHcommStubSetNextChannelCreateResult(std::int32_t result)
{
    g_state.next_channel_create_result = result;
}

void UcmHcommStubSetNextMemImportResult(std::int32_t result)
{
    g_state.next_mem_import_result = result;
}

void UcmHcommStubSetNextWriteResult(std::int32_t result)
{
    g_state.next_write_result = result;
}

void UcmHcommStubSetNextFenceResult(std::int32_t result)
{
    g_state.next_fence_result = result;
}

std::uint32_t UcmHcommStubEndpointCreateCount()
{
    return g_state.endpoint_create_count.load();
}

std::uint32_t UcmHcommStubEndpointDestroyCount()
{
    return g_state.endpoint_destroy_count.load();
}

std::uint32_t UcmHcommStubChannelCreateCount()
{
    return g_state.channel_create_count.load();
}

std::uint32_t UcmHcommStubChannelDestroyCount()
{
    return g_state.channel_destroy_count.load();
}

std::uint32_t UcmHcommStubMemImportCount()
{
    return g_state.mem_import_count.load();
}

std::uint32_t UcmHcommStubMemUnimportCount()
{
    return g_state.mem_unimport_count.load();
}

std::uint32_t UcmHcommStubWriteCount()
{
    return g_state.write_count.load();
}

std::uint32_t UcmHcommStubFenceCount()
{
    return g_state.fence_count.load();
}

HcommResult HcommEndpointCreate(const EndpointDesc* endpoint, EndpointHandle* endpoint_handle)
{
    (void)endpoint;
    ++g_state.endpoint_create_count;
    auto result = Consume(g_state.next_endpoint_create_result);
    if (result != kSuccess) { return static_cast<HcommResult>(result); }
    if (endpoint_handle == nullptr) { return static_cast<HcommResult>(1); }
    auto handle = g_state.next_endpoint_handle.fetch_add(1);
    *endpoint_handle = reinterpret_cast<EndpointHandle>(handle);
    return kSuccess;
}

HcommResult HcommEndpointDestroy(EndpointHandle endpoint_handle)
{
    (void)endpoint_handle;
    ++g_state.endpoint_destroy_count;
    return kSuccess;
}

HcommResult HcommThreadAlloc(CommEngine engine, std::uint32_t thread_num,
                             const std::uint32_t* notify_num_per_thread,
                             ThreadHandle* threads)
{
    (void)engine;
    (void)notify_num_per_thread;
    ++g_state.thread_alloc_count;
    auto result = Consume(g_state.next_thread_alloc_result);
    if (result != kSuccess) { return static_cast<HcommResult>(result); }
    if (threads == nullptr) { return static_cast<HcommResult>(1); }
    for (std::uint32_t i = 0; i < thread_num; ++i) {
        threads[i] = static_cast<ThreadHandle>(g_state.next_thread_handle.fetch_add(1));
    }
    return kSuccess;
}

HcommResult HcommThreadFree(const ThreadHandle* threads, std::uint32_t thread_num)
{
    (void)threads;
    (void)thread_num;
    ++g_state.thread_free_count;
    return kSuccess;
}

HcommResult HcommMemReg(EndpointHandle endpoint_handle, const char* mem_tag,
                        const CommMem* mem, HcommMemHandle* mem_handle)
{
    (void)endpoint_handle;
    (void)mem_tag;
    (void)mem;
    ++g_state.mem_reg_count;
    if (mem_handle == nullptr) { return static_cast<HcommResult>(1); }
    auto handle = g_state.next_mem_handle.fetch_add(1);
    *mem_handle = reinterpret_cast<HcommMemHandle>(handle);
    return kSuccess;
}

HcommResult HcommMemUnreg(EndpointHandle endpoint_handle, HcommMemHandle mem_handle)
{
    (void)endpoint_handle;
    (void)mem_handle;
    ++g_state.mem_unreg_count;
    return kSuccess;
}

HcommResult HcommMemExport(EndpointHandle endpoint_handle, HcommMemHandle mem_handle,
                           void** mem_desc, std::uint32_t* mem_desc_len)
{
    (void)endpoint_handle;
    (void)mem_handle;
    static const std::uint8_t desc[] = {'u', 'c', 'm', 'm', 'e', 'm'};
    if (mem_desc == nullptr || mem_desc_len == nullptr) { return static_cast<HcommResult>(1); }
    *mem_desc = const_cast<std::uint8_t*>(desc);
    *mem_desc_len = sizeof(desc);
    return kSuccess;
}

HcommResult HcommMemImport(EndpointHandle endpoint_handle, const void* mem_desc,
                           std::uint32_t desc_len, CommMem* out_mem)
{
    (void)endpoint_handle;
    ++g_state.mem_import_count;
    auto result = Consume(g_state.next_mem_import_result);
    if (result != kSuccess) { return static_cast<HcommResult>(result); }
    if (mem_desc == nullptr || desc_len == 0 || out_mem == nullptr) {
        return static_cast<HcommResult>(1);
    }
    out_mem->type = COMM_MEM_TYPE_HOST;
    out_mem->addr = const_cast<void*>(mem_desc);
    out_mem->size = desc_len;
    return kSuccess;
}

HcommResult HcommMemUnimport(EndpointHandle endpoint_handle, const void* mem_desc,
                             std::uint32_t desc_len)
{
    (void)endpoint_handle;
    (void)mem_desc;
    (void)desc_len;
    ++g_state.mem_unimport_count;
    return kSuccess;
}

HcommResult HcommChannelCreate(EndpointHandle endpoint_handle, CommEngine engine,
                               HcommChannelDesc* channel_descs,
                               std::uint32_t channel_num, ChannelHandle* channels)
{
    (void)endpoint_handle;
    (void)engine;
    (void)channel_descs;
    ++g_state.channel_create_count;
    auto result = Consume(g_state.next_channel_create_result);
    if (result != kSuccess) { return static_cast<HcommResult>(result); }
    if (channels == nullptr) { return static_cast<HcommResult>(1); }
    for (std::uint32_t i = 0; i < channel_num; ++i) {
        channels[i] = static_cast<ChannelHandle>(g_state.next_channel_handle.fetch_add(1));
    }
    return kSuccess;
}

HcommResult HcommChannelDestroy(const ChannelHandle* channels, std::uint32_t channel_num)
{
    (void)channels;
    (void)channel_num;
    ++g_state.channel_destroy_count;
    return kSuccess;
}

HcommResult HcommChannelGetStatus(const ChannelHandle* channel_list, std::uint32_t list_num,
                                  std::int32_t* status_list)
{
    (void)channel_list;
    if (status_list != nullptr) {
        std::fill(status_list, status_list + list_num, 0);
    }
    return kSuccess;
}

std::int32_t HcommWriteNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                   void* dst, const void* src, std::uint64_t len)
{
    (void)thread;
    (void)channel;
    ++g_state.write_count;
    auto result = Consume(g_state.next_write_result);
    if (result != kSuccess) { return result; }
    if (dst == nullptr || src == nullptr || len == 0) { return 1; }
    std::memcpy(dst, src, static_cast<std::size_t>(len));
    return kSuccess;
}

std::int32_t HcommReadNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                  void* dst, const void* src, std::uint64_t len)
{
    return HcommWriteNbiOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommReadOnThread(ThreadHandle thread, ChannelHandle channel,
                               void* dst, const void* src, std::uint64_t len)
{
    return HcommWriteNbiOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommWriteOnThread(ThreadHandle thread, ChannelHandle channel,
                                void* dst, const void* src, std::uint64_t len)
{
    return HcommWriteNbiOnThread(thread, channel, dst, src, len);
}

std::int32_t HcommChannelFenceOnThread(ThreadHandle thread, ChannelHandle channel)
{
    (void)thread;
    (void)channel;
    ++g_state.fence_count;
    return Consume(g_state.next_fence_result);
}

std::int32_t HcommBatchModeStart(const char* batch_tag)
{
    (void)batch_tag;
    return kSuccess;
}

std::int32_t HcommBatchModeEnd(const char* batch_tag)
{
    (void)batch_tag;
    return kSuccess;
}

}  // extern "C"

