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
#ifndef KV_SEMANTICS_RUNTIME_CUDA_STATUS_H
#define KV_SEMANTICS_RUNTIME_CUDA_STATUS_H

#include <cuda_runtime_api.h>
#include "kv_types.h"

namespace kv::runtime {

inline Status FromCuda(cudaError_t result)
{
    if (result == cudaSuccess) { return Status::OK(); }
    const auto code = result == cudaErrorInvalidDevice || result == cudaErrorInvalidValue
                          ? StatusCode::INVALID_ARGUMENT
                          : StatusCode::INTERNAL_ERROR;
    return Status::Error(code, cudaGetErrorString(result));
}

}  // namespace kv::runtime

#endif  // KV_SEMANTICS_RUNTIME_CUDA_STATUS_H
