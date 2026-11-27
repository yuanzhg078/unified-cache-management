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
#include <cuda_runtime_api.h>
#include <string>
#include "cuda_buffer.h"
#include "cuda_status.h"
#include "cuda_trans.h"
#include "device.h"
#include "event.h"

namespace kv::runtime {

Status SynchronizeEvent(std::uintptr_t eventHandle)
{
    if (eventHandle == 0) { return Status::OK(); }
    return FromCuda(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(eventHandle)));
}

Status Device::Init() { return Status::OK(); }

Status Device::Setup(int32_t deviceId) { return FromCuda(cudaSetDevice(deviceId)); }

Status Device::Reset(int32_t deviceId)
{
    // The serving process owns the shared CUDA context.
    if (deviceId < 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "invalid device id(" + std::to_string(deviceId) + ")");
    }
    return Status::OK();
}

Status Device::Finalize() { return Status::OK(); }

std::unique_ptr<Buffer> Device::MakeBuffer() { return std::make_unique<CudaBuffer>(); }

std::unique_ptr<Trans> Device::MakeTrans() { return std::make_unique<CudaTrans>(); }

}  // namespace kv::runtime
