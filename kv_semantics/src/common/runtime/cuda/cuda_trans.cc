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
#include "cuda_status.h"
#include "cuda_trans.h"

namespace kv::runtime {

Status CudaTrans::DeviceToHost(void* device, void* host, size_t size)
{
    return FromCuda(cudaMemcpy(host, device, size, cudaMemcpyDeviceToHost));
}

Status CudaTrans::DeviceToHost(void* device[], void* host[], size_t size, size_t number)
{
    for (size_t i = 0; i < number; ++i) {
        auto status = DeviceToHost(device[i], host[i], size);
        if (!status.ok()) { return status; }
    }
    return Status::OK();
}

Status CudaTrans::DeviceToHost(void* device[], void* host, size_t size, size_t number)
{
    auto* bytes = static_cast<char*>(host);
    for (size_t i = 0; i < number; ++i) {
        auto status = DeviceToHost(device[i], bytes + i * size, size);
        if (!status.ok()) { return status; }
    }
    return Status::OK();
}

Status CudaTrans::HostToDevice(void* host, void* device, size_t size)
{
    const auto result = cudaMemcpy(device, host, size, cudaMemcpyHostToDevice);
    if (result != cudaSuccess) { return FromCuda(result); }
    // Keep the pageable host buffer alive until the device copy has completed.
    return FromCuda(cudaStreamSynchronize(nullptr));
}

Status CudaTrans::HostToDevice(void* host[], void* device[], size_t size, size_t number)
{
    for (size_t i = 0; i < number; ++i) {
        auto status = HostToDevice(host[i], device[i], size);
        if (!status.ok()) { return status; }
    }
    return Status::OK();
}

Status CudaTrans::HostToDevice(void* host, void* device[], size_t size, size_t number)
{
    auto* bytes = static_cast<char*>(host);
    for (size_t i = 0; i < number; ++i) {
        auto status = HostToDevice(bytes + i * size, device[i], size);
        if (!status.ok()) { return status; }
    }
    return Status::OK();
}

}  // namespace kv::runtime
