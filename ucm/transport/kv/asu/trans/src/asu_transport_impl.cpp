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
#include "asu_transport_impl.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include "asu_transport/asu_transport.h"
#include "asu_transport/types.h"

namespace UC::ASU {
namespace {

std::string ProtocolToString(Protocol protocol)
{
    switch (protocol) {
        case Protocol::UB:
            return "ub";
        case Protocol::ROCE:
            return "roce";
        case Protocol::TCP:
            return "tcp";
        default:
            return "roce";
    }
}

void MergeAttrs(std::unordered_map<std::string, std::string>& dst,
                const std::unordered_map<std::string, std::string>& src)
{
    for (const auto& item : src) { dst[item.first] = item.second; }
}

void SetAttrIfMissing(std::unordered_map<std::string, std::string>& attrs,
                      const std::string& key, std::string value)
{
    if (value.empty()) { return; }
    if (attrs.find(key) == attrs.end()) { attrs.emplace(key, std::move(value)); }
}

ConnectionMemType ToConnectionMemType(MemoryType type)
{
    return type == MemoryType::ASCEND_DEVICE ? ConnectionMemType::DEVICE
                                             : ConnectionMemType::HOST;
}

RegisterMemoryDesc ToRegisterMemoryDesc(const MemoryRegion& region)
{
    RegisterMemoryDesc desc;
    desc.memory_type = ToConnectionMemType(region.memory_type);
    desc.addr = static_cast<std::uintptr_t>(region.addr);
    desc.size = static_cast<std::size_t>(region.size);
    return desc;
}

}  // namespace

AsuTransportImpl::~AsuTransportImpl() { Shutdown(); }

Status AsuTransportImpl::Init(const TransportConfig& config)
{
    if (worker_.joinable()) { return Status::OK(); }

    config_ = config;
    auto status = InitConnections();
    if (!status.ok()) { return status; }

    auto queue_depth =
        std::max<std::size_t>(2, static_cast<std::size_t>(config_.max_inflight_tasks));
    execute_queue_.Setup(queue_depth + 1);
    stop_.store(false, std::memory_order_release);
    worker_ = std::thread(&AsuTransportImpl::WorkerLoop, this);
    return Status::OK();
}

Status AsuTransportImpl::Shutdown()
{
    ShutdownConnections();
    if (!worker_.joinable()) { return Status::OK(); }

    // TODO: Drain task queue and fail all pending tasks

    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) { worker_.join(); }
    return Status::OK();
}

std::uint32_t AsuTransportImpl::SelectEndpointQpNum(const TransportConfig& config)
{
    return std::max({config.query_qp_num, config.load_qp_num, config.store_qp_num});
}

Status AsuTransportImpl::InitConnections()
{
    if (!config_.preconnect) { return Status::OK(); }

    connection_manager_ = CreateConnectionManager(
        ConnectionManagerConfig{ConnectionBackendType::HCOMM, config_.attrs});
    if (!connection_manager_) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "connection manager is null");
    }
    auto status = connection_manager_->Initialize();
    if (!status.ok()) {
        connection_manager_.reset();
        return status;
    }

    endpoint_connections_.clear();
    endpoint_connections_.reserve(config_.endpoints.size());
    const auto qp_num = SelectEndpointQpNum(config_);
    const auto timeout_ms = static_cast<std::uint32_t>(
        std::max({config_.query_timeout_ms, config_.load_timeout_ms, config_.store_timeout_ms}));

    for (const auto& endpoint : config_.endpoints) {
        auto merged_attrs = config_.attrs;
        MergeAttrs(merged_attrs, endpoint.attrs);
        SetAttrIfMissing(merged_attrs, "protocol", ProtocolToString(endpoint.protocol));
        SetAttrIfMissing(merged_attrs, "remote_comm_id", endpoint.ip);
        SetAttrIfMissing(merged_attrs, "remote_ip", endpoint.ip);
        if (endpoint.port != 0) {
            SetAttrIfMissing(merged_attrs, "port", std::to_string(endpoint.port));
        }
        if (endpoint.device_id >= 0) {
            SetAttrIfMissing(merged_attrs, "remote_phy_device_id",
                             std::to_string(static_cast<std::uint64_t>(endpoint.device_id)));
        }

        auto local_ip_it = merged_attrs.find("local_ip");
        const std::string local_ip =
            local_ip_it == merged_attrs.end() ? std::string{} : local_ip_it->second;
        const auto remote_ip = endpoint.ip;
        const auto port = endpoint.port;

        EndpointConnection endpoint_connection;
        endpoint_connection.endpoint = endpoint;
        CreateConnectionRequest request;
        request.local_ip = local_ip;
        request.remote_ip = remote_ip;
        request.port = port;
        request.qp_num = qp_num;
        request.timeout_ms = timeout_ms;
        request.attrs = std::move(merged_attrs);
        status = connection_manager_->CreateConnection(request, endpoint_connection.endpoint_handle,
                                                       endpoint_connection.handles);
        if (!status.ok()) {
            ShutdownConnections();
            return status;
        }

        endpoint_connections_.emplace_back(std::move(endpoint_connection));
    }
    return Status::OK();
}

void AsuTransportImpl::ShutdownConnections()
{
    {
        std::lock_guard<std::mutex> lock(registered_mu_);
        registered_regions_.clear();
    }
    if (connection_manager_) {
        for (auto& endpoint_connection : endpoint_connections_) {
            if (!endpoint_connection.handles.empty()) {
                (void)connection_manager_->DeleteConnections(endpoint_connection.handles);
                endpoint_connection.handles.clear();
            }
        }
        connection_manager_->Finalize();
        connection_manager_.reset();
    }
    endpoint_connections_.clear();
}

Status AsuTransportImpl::CheckHealth()
{
    // TODO: real health check
    return Status::OK();
}

Status AsuTransportImpl::Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                               QueryResult& result)
{
    TaskId task_id{kInvalidTaskId};
    auto status = QueryAsync(keys, options, task_id);
    if (!status.ok()) { return status; }

    TaskResult task_result;
    const auto timeout_ms = options.timeout_ms == 0 ? config_.query_timeout_ms : options.timeout_ms;
    status = Wait(task_id, timeout_ms, task_result);
    if (!status.ok()) { return status; }
    if (task_result.query_result.has_value()) { result = *task_result.query_result; }
    return task_result.status;
}

Status AsuTransportImpl::QueryAsync(const std::vector<CacheKey>& keys, const QueryOptions& options,
                                    TaskId& task_id)
{
    auto ctx = std::make_unique<TransportTaskContext>();
    ctx->op_type = TransportOpType::QUERY;
    ctx->keys = BatchView<CacheKey>{keys.data(), keys.size()};
    ctx->query_options = options;
    ctx->entry_status.assign(keys.size(), Status::OK());
    return SubmitAsync(std::move(ctx), task_id);
}

Status AsuTransportImpl::LoadAsync(const std::vector<KVBuffer>& entries, TaskId& task_id)
{
    auto ctx = std::make_unique<TransportTaskContext>();
    ctx->op_type = TransportOpType::LOAD;
    ctx->entries = BatchView<KVBuffer>{entries.data(), entries.size()};
    ctx->entry_status.assign(entries.size(), Status::OK());
    return SubmitAsync(std::move(ctx), task_id);
}

Status AsuTransportImpl::StoreAsync(const std::vector<KVBuffer>& entries, TaskId& task_id)
{
    auto ctx = std::make_unique<TransportTaskContext>();
    ctx->op_type = TransportOpType::STORE;
    ctx->entries = BatchView<KVBuffer>{entries.data(), entries.size()};
    ctx->entry_status.assign(entries.size(), Status::OK());
    return SubmitAsync(std::move(ctx), task_id);
}

Status AsuTransportImpl::DeleteAsync(const std::vector<CacheKey>& keys, TaskId& task_id)
{
    auto ctx = std::make_unique<TransportTaskContext>();
    ctx->op_type = TransportOpType::DELETE;
    ctx->keys = BatchView<CacheKey>{keys.data(), keys.size()};
    ctx->entry_status.assign(keys.size(), Status::OK());
    return SubmitAsync(std::move(ctx), task_id);
}

Status AsuTransportImpl::Cancel(TaskId task_id)
{
    return Status::Error(StatusCode::INTERNAL_ERROR, "cancel is not supported now");
}

Status AsuTransportImpl::Check(TaskId task_id, TaskResult& result)
{
    auto ctx = task_manager_.Get(task_id);
    if (!ctx) { return Status::Error(StatusCode::TASK_NOT_FOUND, "transport task not found"); }

    std::lock_guard<std::mutex> lock(ctx->wait_mu);
    BuildResult(*ctx, result);
    if (!ctx->Done()) {
        result.status = Status::Error(StatusCode::IN_PROGRESS, "transport task in progress");
    }
    return Status::OK();
}

Status AsuTransportImpl::Wait(TaskId task_id, std::uint64_t timeout_ms, TaskResult& result)
{
    auto ctx = task_manager_.Get(task_id);
    if (!ctx) { return Status::Error(StatusCode::TASK_NOT_FOUND, "transport task not found"); }

    std::unique_lock<std::mutex> lock(ctx->wait_mu);
    const bool done = timeout_ms == 0
                          ? (ctx->cv.wait(lock, [ctx] { return ctx->Done(); }), true)
                          : ctx->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                             [ctx] { return ctx->Done(); });
    BuildResult(*ctx, result);
    if (!done) {
        result.status = Status::Error(StatusCode::TIMEOUT, "transport task wait timeout");
        return result.status;
    }
    lock.unlock();
    task_manager_.Remove(task_id);
    return Status::OK();
}

Status AsuTransportImpl::RegisterRegions(const std::vector<MemoryRegion>& regions,
                                         std::vector<RegisterHandleResult>& results)
{
    results.clear();
    results.assign(regions.size(), RegisterHandleResult{Status::OK(), kInvalidMRHandle});
    if (!connection_manager_) {
        auto status = Status::Error(StatusCode::NOT_INITIALIZED,
                                    "transport connection manager is not initialized");
        for (auto& result : results) { result.status = status; }
        return status;
    }
    if (endpoint_connections_.empty()) {
        auto status = Status::Error(StatusCode::NOT_INITIALIZED,
                                    "transport has no endpoint connections");
        for (auto& result : results) { result.status = status; }
        return status;
    }

    bool any_failed = false;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        if (region.addr == 0 || region.size == 0) {
            results[i].status =
                Status::Error(StatusCode::INVALID_ARGUMENT, "memory region addr or size is invalid");
            any_failed = true;
            continue;
        }

        RegisteredRegionState state;
        state.region = region;
        const auto desc = ToRegisterMemoryDesc(region);
        bool failed = false;
        for (const auto& endpoint_connection : endpoint_connections_) {
            if (endpoint_connection.endpoint_handle == kInvalidConnectionEndpointHandle) {
                results[i].status = Status::Error(StatusCode::INTERNAL_ERROR,
                                                  "endpoint handle is invalid");
                failed = true;
                break;
            }
            std::vector<CommMemHandle> memory_handles;
            auto status = connection_manager_->RegisterMemory(endpoint_connection.endpoint_handle,
                                                              {desc}, memory_handles);
            if (!status.ok()) {
                results[i].status = status;
                failed = true;
                break;
            }
            if (memory_handles.size() != 1 || memory_handles[0] == kInvalidCommMemHandle) {
                results[i].status = Status::Error(StatusCode::INTERNAL_ERROR,
                                                  "register memory returned invalid handle");
                failed = true;
                break;
            }
            state.unregister_descs.push_back(
                UnregisterMemoryDesc{endpoint_connection.endpoint_handle, memory_handles[0]});
        }

        if (failed) {
            if (!state.unregister_descs.empty()) {
                (void)connection_manager_->UnregisterMemory(state.unregister_descs);
            }
            any_failed = true;
            continue;
        }

        std::lock_guard<std::mutex> lock(registered_mu_);
        auto handle = next_mr_handle_++;
        if (handle == kInvalidMRHandle) { handle = next_mr_handle_++; }
        registered_regions_.emplace(handle, std::move(state));
        results[i].handle = handle;
    }

    if (any_failed) {
        return Status::Error(StatusCode::PARTIAL_FAILED, "some memory regions failed to register");
    }
    return Status::OK();
}

Status AsuTransportImpl::BindRegisteredRegions(const std::vector<RegisteredMemory>& regions,
                                               std::vector<RegisterHandleResult>& results)
{
    results.clear();
    results.assign(regions.size(), RegisterHandleResult{Status::OK(), kInvalidMRHandle});
    // TODO:
    return Status::OK();
}

Status AsuTransportImpl::UnregisterRegions(const std::vector<MRHandle>& handles)
{
    if (!connection_manager_) { return Status::OK(); }
    std::vector<UnregisterMemoryDesc> descs;
    {
        std::lock_guard<std::mutex> lock(registered_mu_);
        for (auto handle : handles) {
            auto it = registered_regions_.find(handle);
            if (it == registered_regions_.end()) { continue; }
            descs.insert(descs.end(), it->second.unregister_descs.begin(),
                         it->second.unregister_descs.end());
            registered_regions_.erase(it);
        }
    }
    if (!descs.empty()) {
        auto statuses = connection_manager_->UnregisterMemory(descs);
        for (const auto& status : statuses) {
            if (!status.ok()) { return status; }
        }
    }
    return Status::OK();
}

Status AsuTransportImpl::SubmitAsync(std::unique_ptr<TransportTaskContext> ctx, TaskId& task_id)
{
    auto status = task_manager_.Submit(std::move(ctx), task_id);
    if (!status.ok()) { return status; }

    auto raw_ctx = task_manager_.Get(task_id);
    if (!raw_ctx) {
        task_id = kInvalidTaskId;
        return Status::Error(StatusCode::INTERNAL_ERROR, "transport task disappeared after submit");
    }

    std::lock_guard<std::mutex> lock(producer_mu_);
    if (!execute_queue_.TryPush(std::move(raw_ctx))) {
        task_manager_.Remove(task_id);
        task_id = kInvalidTaskId;
        return Status::Error(StatusCode::RESOURCE_BUSY, "transport task queue is full");
    }
    return Status::OK();
}

void AsuTransportImpl::WorkerLoop()
{
    execute_queue_.ConsumerLoop(stop_, [this](TransportTaskContextPtr ctx) {
        if (!ctx) { return; }
        CompleteTask(ctx);
    });
}

void AsuTransportImpl::CompleteTask(const TransportTaskContextPtr& ctx)
{
    // TODO: do REAL work here
    TransportTaskState expected = TransportTaskState::PENDING;
    if (!ctx->state.compare_exchange_strong(expected, TransportTaskState::INFLIGHT,
                                            std::memory_order_acq_rel)) {
        if (ctx->state.load(std::memory_order_acquire) == TransportTaskState::CANCELED) {
            ctx->cv.notify_all();
        }
        return;
    }

    std::lock_guard<std::mutex> lock(ctx->wait_mu);
    if (ctx->op_type == TransportOpType::QUERY) {
        ctx->query_result.exists.assign(ctx->keys.size, 0);
        ctx->query_result.prefix_hit_keys = 0;
    }
    ctx->final_status = Status::OK();
    ctx->state.store(TransportTaskState::COMPLETED, std::memory_order_release);
    ctx->cv.notify_all();
}

void AsuTransportImpl::BuildResult(const TransportTaskContext& ctx, TaskResult& result)
{
    result.status = ctx.final_status;
    result.entry_status = ctx.entry_status;
    result.query_result.reset();
    if (ctx.op_type == TransportOpType::QUERY) { result.query_result = ctx.query_result; }
}

std::unique_ptr<AsuTransport> CreateAsuTransport() { return std::make_unique<AsuTransportImpl>(); }

}  // namespace UC::ASU
