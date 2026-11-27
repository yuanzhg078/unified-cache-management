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
#include "cuda_buffer.h"
#include "cuda_status.h"

namespace kv::runtime {

std::shared_ptr<void> CudaBuffer::MakeDeviceBuffer(size_t size)
{
    void* device = nullptr;
    if (cudaMalloc(&device, size) != cudaSuccess) { return nullptr; }
    return std::shared_ptr<void>(device, [](void* ptr) { (void)cudaFree(ptr); });
}

std::shared_ptr<void> CudaBuffer::MakeHostBuffer(size_t size)
{
    void* host = nullptr;
    if (cudaMallocHost(&host, size) != cudaSuccess) { return nullptr; }
    return std::shared_ptr<void>(host, [](void* ptr) { (void)cudaFreeHost(ptr); });
}

std::shared_ptr<void> CudaBuffer::MakeHostMappedDeviceBuffer(size_t size, void** pDevice)
{
    if (pDevice) { *pDevice = nullptr; }
    void* host = nullptr;
    if (cudaHostAlloc(&host, size, cudaHostAllocMapped) != cudaSuccess) { return nullptr; }
    auto owner = std::shared_ptr<void>(host, [](void* ptr) { (void)cudaFreeHost(ptr); });
    if (pDevice && cudaHostGetDevicePointer(pDevice, host, 0) != cudaSuccess) {
        *pDevice = nullptr;
        return nullptr;
    }
    return owner;
}

Status Memset(void* ptr, std::size_t size, std::int32_t value)
{
    const auto result = cudaMemset(ptr, value, size);
    if (result != cudaSuccess) { return FromCuda(result); }
    return FromCuda(cudaStreamSynchronize(nullptr));
}

}  // namespace kv::runtime
