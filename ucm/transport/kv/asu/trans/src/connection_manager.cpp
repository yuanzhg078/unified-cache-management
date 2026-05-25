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
#include "asu_transport/connection_manager.h"

#include <mutex>
#include <unordered_set>
#include <utility>
#include "connection_backend.h"

namespace UC::ASU {
namespace {

Status Invalid(std::string message)
{
    return Status::Error(StatusCode::INVALID_ARGUMENT, std::move(message));
}

Status NotInitialized()
{
    return Status::Error(StatusCode::NOT_INITIALIZED, "connection manager is not initialized");
}

std::vector<Status> SameStatus(std::size_t count, const Status& status)
{
    std::vector<Status> statuses;
    statuses.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { statuses.emplace_back(status); }
    return statuses;
}

}  // namespace

class ConnectionManager::Impl {
public:
    explicit Impl(ConnectionManagerConfig config)
        : config_(std::move(config)), backend_(CreateConnectionBackend(config_.backend_type))
    {
    }

    ~Impl() { Finalize(); }

    Status Initialize()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (initialized_) { return Status::OK(); }
        if (!backend_) {
            return Status::Error(StatusCode::INTERNAL_ERROR, "connection backend is null");
        }
        auto status = backend_->Initialize(config_);
        initialized_ = status.ok();
        return status;
    }

    void Finalize()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return; }
        backend_->Finalize();
        memory_handles_.clear();
        connection_handles_.clear();
        initialized_ = false;
    }

    Status CreateConnection(const CreateConnectionRequest& request,
                            std::vector<ConnectionHandle>& connection_handles)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return NotInitialized(); }
        if (request.qp_num == 0) { return Invalid("qp_num must be greater than 0"); }
        if (request.timeout_ms == 0) { return Invalid("timeout_ms must be greater than 0"); }

        connection_handles.clear();
        auto status = backend_->CreateConnection(request, connection_handles);
        if (!status.ok()) { return status; }
        if (connection_handles.size() != request.qp_num) {
            connection_handles.clear();
            return Status::Error(StatusCode::INTERNAL_ERROR,
                                 "backend returned unexpected connection handle count");
        }
        for (auto handle : connection_handles) {
            if (handle == kInvalidConnectionHandle) {
                connection_handles.clear();
                return Status::Error(StatusCode::INTERNAL_ERROR,
                                     "backend returned invalid connection handle");
            }
            connection_handles_.insert(handle);
        }
        return Status::OK();
    }

    std::vector<Status> DeleteConnections(const std::vector<ConnectionHandle>& connection_handles)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return SameStatus(connection_handles.size(), NotInitialized()); }
        auto invalid = ValidateConnectionHandles(connection_handles);
        if (!invalid.ok()) { return SameStatus(connection_handles.size(), invalid); }

        auto statuses = backend_->DeleteConnections(connection_handles);
        NormalizeBatchStatusSize(statuses, connection_handles.size());
        for (std::size_t i = 0; i < connection_handles.size(); ++i) {
            if (statuses[i].ok()) { connection_handles_.erase(connection_handles[i]); }
        }
        return statuses;
    }

    std::vector<Status> Send(const std::vector<SendIoBatch>& io_batches,
                             std::uint32_t kernel_count, std::uint32_t quiet_count)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return SameStatus(io_batches.size(), NotInitialized()); }
        if (kernel_count == 0) {
            return SameStatus(io_batches.size(), Invalid("kernel_count must be greater than 0"));
        }
        if (quiet_count == 0) {
            return SameStatus(io_batches.size(), Invalid("quiet_count must be greater than 0"));
        }
        for (const auto& batch : io_batches) {
            auto status = ValidateConnectionHandle(batch.connection_handle);
            if (!status.ok()) { return SameStatus(io_batches.size(), status); }
            if (batch.send_buffer == nullptr) {
                return SameStatus(io_batches.size(), Invalid("send_buffer is null"));
            }
            if (batch.flag_buffer == nullptr) {
                return SameStatus(io_batches.size(), Invalid("flag_buffer is null"));
            }
        }

        auto statuses = backend_->Send(io_batches, kernel_count, quiet_count);
        NormalizeBatchStatusSize(statuses, io_batches.size());
        return statuses;
    }

    Status RegisterMemory(ConnectionHandle connection_handle,
                          const std::vector<RegisterMemoryDesc>& memory_descs,
                          std::vector<CommMemHandle>& memory_handles)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return NotInitialized(); }
        auto status = ValidateConnectionHandle(connection_handle);
        if (!status.ok()) { return status; }
        if (memory_descs.empty()) { return Invalid("memory_descs is empty"); }
        for (const auto& desc : memory_descs) {
            if (desc.addr == 0) { return Invalid("memory addr is null"); }
            if (desc.size == 0) { return Invalid("memory size must be greater than 0"); }
        }

        memory_handles.clear();
        status = backend_->RegisterMemory(connection_handle, memory_descs, memory_handles);
        if (!status.ok()) { return status; }
        if (memory_handles.size() != memory_descs.size()) {
            memory_handles.clear();
            return Status::Error(StatusCode::INTERNAL_ERROR,
                                 "backend returned unexpected memory handle count");
        }
        for (auto handle : memory_handles) {
            if (handle == kInvalidCommMemHandle) {
                memory_handles.clear();
                return Status::Error(StatusCode::INTERNAL_ERROR,
                                     "backend returned invalid memory handle");
            }
            memory_handles_.insert(handle);
        }
        return Status::OK();
    }

    std::vector<Status> UnregisterMemory(const std::vector<UnregisterMemoryDesc>& memory_descs)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) { return SameStatus(memory_descs.size(), NotInitialized()); }
        for (const auto& desc : memory_descs) {
            auto status = ValidateConnectionHandle(desc.connection_handle);
            if (!status.ok()) { return SameStatus(memory_descs.size(), status); }
            if (desc.memory_handle == kInvalidCommMemHandle) {
                return SameStatus(memory_descs.size(), Invalid("memory_handle is invalid"));
            }
            if (memory_handles_.find(desc.memory_handle) == memory_handles_.end()) {
                return SameStatus(memory_descs.size(),
                                  Status::Error(StatusCode::NOT_FOUND,
                                                "memory_handle is not registered by this manager"));
            }
        }

        auto statuses = backend_->UnregisterMemory(memory_descs);
        NormalizeBatchStatusSize(statuses, memory_descs.size());
        for (std::size_t i = 0; i < memory_descs.size(); ++i) {
            if (statuses[i].ok()) { memory_handles_.erase(memory_descs[i].memory_handle); }
        }
        return statuses;
    }

private:
    Status ValidateConnectionHandle(ConnectionHandle handle) const
    {
        if (handle == kInvalidConnectionHandle) { return Invalid("connection_handle is invalid"); }
        if (connection_handles_.find(handle) == connection_handles_.end()) {
            return Status::Error(StatusCode::NOT_FOUND,
                                 "connection_handle is not created by this manager");
        }
        return Status::OK();
    }

    Status ValidateConnectionHandles(const std::vector<ConnectionHandle>& handles) const
    {
        for (auto handle : handles) {
            auto status = ValidateConnectionHandle(handle);
            if (!status.ok()) { return status; }
        }
        return Status::OK();
    }

    static void NormalizeBatchStatusSize(std::vector<Status>& statuses, std::size_t expected)
    {
        if (statuses.size() == expected) { return; }
        statuses.assign(expected, Status::Error(StatusCode::INTERNAL_ERROR,
                                                "backend returned unexpected status count"));
    }

    ConnectionManagerConfig config_;
    std::unique_ptr<ConnectionBackend> backend_;
    bool initialized_{false};
    std::unordered_set<ConnectionHandle> connection_handles_;
    std::unordered_set<CommMemHandle> memory_handles_;
    std::mutex mu_;
};

ConnectionManager::ConnectionManager() : ConnectionManager(ConnectionManagerConfig{}) {}

ConnectionManager::ConnectionManager(ConnectionManagerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

ConnectionManager::~ConnectionManager() = default;

ConnectionManager::ConnectionManager(ConnectionManager&&) noexcept = default;

ConnectionManager& ConnectionManager::operator=(ConnectionManager&&) noexcept = default;

Status ConnectionManager::Initialize()
{
    return impl_->Initialize();
}

void ConnectionManager::Finalize()
{
    impl_->Finalize();
}

Status ConnectionManager::CreateConnection(const CreateConnectionRequest& request,
                                           std::vector<ConnectionHandle>& connection_handles)
{
    return impl_->CreateConnection(request, connection_handles);
}

std::vector<Status> ConnectionManager::DeleteConnections(
    const std::vector<ConnectionHandle>& connection_handles)
{
    return impl_->DeleteConnections(connection_handles);
}

std::vector<Status> ConnectionManager::Send(const std::vector<SendIoBatch>& io_batches,
                                            std::uint32_t kernel_count,
                                            std::uint32_t quiet_count)
{
    return impl_->Send(io_batches, kernel_count, quiet_count);
}

Status ConnectionManager::RegisterMemory(ConnectionHandle connection_handle,
                                         const std::vector<RegisterMemoryDesc>& memory_descs,
                                         std::vector<CommMemHandle>& memory_handles)
{
    return impl_->RegisterMemory(connection_handle, memory_descs, memory_handles);
}

std::vector<Status> ConnectionManager::UnregisterMemory(
    const std::vector<UnregisterMemoryDesc>& memory_descs)
{
    return impl_->UnregisterMemory(memory_descs);
}

std::unique_ptr<ConnectionManager> CreateConnectionManager(ConnectionManagerConfig config)
{
    return std::make_unique<ConnectionManager>(std::move(config));
}

}  // namespace UC::ASU
