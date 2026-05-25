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
#include "hcomm/hcomm_res_defs.h"

namespace UC::ASU {

class HcommProxy {
public:
    static HcommResult EndpointCreate(const EndpointDesc* endpoint,
                                      EndpointHandle* endpoint_handle);
    static HcommResult EndpointDestroy(EndpointHandle endpoint_handle);
    static HcommResult MemReg(EndpointHandle endpoint_handle, const char* mem_tag,
                              const CommMem* mem, HcommMemHandle* mem_handle);
    static HcommResult MemUnreg(EndpointHandle endpoint_handle, HcommMemHandle mem_handle);
    static HcommResult MemExport(EndpointHandle endpoint_handle, HcommMemHandle mem_handle,
                                 void** mem_desc, std::uint32_t* mem_desc_len);
    static HcommResult MemImport(EndpointHandle endpoint_handle, const void* mem_desc,
                                 std::uint32_t desc_len, CommMem* out_mem);
    static HcommResult MemUnimport(EndpointHandle endpoint_handle, const void* mem_desc,
                                   std::uint32_t desc_len);
    static HcommResult ChannelCreate(EndpointHandle endpoint_handle, CommEngine engine,
                                     HcommChannelDesc* channel_descs, std::uint32_t channel_num,
                                     ChannelHandle* channels);
    static HcommResult ChannelDestroy(const ChannelHandle* channels, std::uint32_t channel_num);
    static HcommResult ChannelGetStatus(const ChannelHandle* channel_list,
                                        std::uint32_t list_num, std::int32_t* status_list);
    static HcommResult ThreadAlloc(CommEngine engine, std::uint32_t thread_num,
                                   const std::uint32_t* notify_num_per_thread,
                                   ThreadHandle* threads);
    static HcommResult ThreadFree(const ThreadHandle* threads, std::uint32_t thread_num);
    static std::int32_t WriteNbiOnThread(ThreadHandle thread,
                                         ChannelHandle channel,
                                         void* dst, const void* src, std::uint64_t len);
    static std::int32_t ReadNbiOnThread(ThreadHandle thread, ChannelHandle channel,
                                        void* dst, const void* src, std::uint64_t len);
    static std::int32_t ReadOnThread(ThreadHandle thread, ChannelHandle channel,
                                     void* dst, const void* src, std::uint64_t len);
    static std::int32_t WriteOnThread(ThreadHandle thread,
                                      ChannelHandle channel,
                                      void* dst, const void* src, std::uint64_t len);
    static std::int32_t ChannelFenceOnThread(ThreadHandle thread, ChannelHandle channel);
    static std::int32_t BatchModeStart(const char* batch_tag);
    static std::int32_t BatchModeEnd(const char* batch_tag);
};

}  // namespace UC::ASU
