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

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "asu_transport/asu_transport.h"
#include "asu_transport/connection_manager.h"
#include "template/spsc_ring_queue.h"
#include "transport_task_manager.h"

namespace UC::ASU {

class AsuTransportImpl final : public AsuTransport {
public:
    AsuTransportImpl() = default;
    ~AsuTransportImpl() override;

    Status Init(const TransportConfig& config) override;
    Status Shutdown() override;

    Status CheckHealth() override;

    Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                 QueryResult& result) override;
    Status QueryAsync(const std::vector<CacheKey>& keys, const QueryOptions& options,
                      TaskId& task_id) override;
    Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& task_id) override;
    Status StoreAsync(const std::vector<KVBuffer>& entries, TaskId& task_id) override;
    Status DeleteAsync(const std::vector<CacheKey>& keys, TaskId& task_id) override;

    Status Cancel(TaskId task_id) override;
    Status Check(TaskId task_id, TaskResult& result) override;
    Status Wait(TaskId task_id, std::uint64_t timeout_ms, TaskResult& result) override;

    Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                           std::vector<RegisterHandleResult>& results) override;

    Status BindRegisteredRegions(const std::vector<RegisteredMemory>& regions,
                                 std::vector<RegisterHandleResult>& results) override;

    Status UnregisterRegions(const std::vector<MRHandle>& handles) override;

private:
    struct EndpointConnection {
        AsuEndpoint endpoint;
        ConnectionEndpointHandle endpoint_handle{kInvalidConnectionEndpointHandle};
        std::vector<ConnectionHandle> handles;
    };
    struct RegisteredRegionState {
        MemoryRegion region;
        std::vector<UnregisterMemoryDesc> unregister_descs;
    };

    using TransportTaskContextPtr = std::shared_ptr<TransportTaskContext>;
    Status InitConnections();
    void ShutdownConnections();
    static std::uint32_t SelectEndpointQpNum(const TransportConfig& config);
    Status SubmitAsync(std::unique_ptr<TransportTaskContext> ctx, TaskId& task_id);
    void WorkerLoop();
    void CompleteTask(const TransportTaskContextPtr& ctx);
    void BuildResult(const TransportTaskContext& ctx, TaskResult& result);

    TransportConfig config_;
    std::unique_ptr<ConnectionManager> connection_manager_;
    std::vector<EndpointConnection> endpoint_connections_;

    TransportTaskManager task_manager_;
    // TODO: optimize spsc pattern or just submit to RDMA/UB directly ?
    UC::SpscRingQueue<TransportTaskContextPtr> execute_queue_;
    std::mutex producer_mu_;

    std::thread worker_;
    std::atomic_bool stop_{false};

    std::mutex registered_mu_;
    MRHandle next_mr_handle_{1};
    std::unordered_map<MRHandle, RegisteredRegionState> registered_regions_;
};

}  // namespace UC::ASU
