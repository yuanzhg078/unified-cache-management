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
#include "hcomm_backend.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <vector>
#include <utility>
#include "hcomm_proxy.h"

namespace UC::ASU {
namespace {

constexpr HcommResult kHcommSuccess = 0;
constexpr HcommResult kHcommNotSupport = 9;

Status HcommStatus(HcommResult ret, std::string op)
{
    if (ret == kHcommSuccess) { return Status::OK(); }
    auto code = ret == kHcommNotSupport ? StatusCode::UNSUPPORTED : StatusCode::IO_ERROR;
    return Status::Error(code, std::move(op) + " failed, hcomm ret=" + std::to_string(ret));
}

Status Invalid(std::string message)
{
    return Status::Error(StatusCode::INVALID_ARGUMENT, std::move(message));
}

std::vector<Status> SameStatus(std::size_t count, const Status& status)
{
    std::vector<Status> statuses;
    statuses.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { statuses.emplace_back(status); }
    return statuses;
}

void* ValueToPtr(std::uint64_t value)
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string Trim(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
        return !is_space(static_cast<unsigned char>(ch));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
        return !is_space(static_cast<unsigned char>(ch));
    }).base(), value.end());
    return value;
}

std::string NormalizeConfigKey(std::string key)
{
    key = ToLower(Trim(std::move(key)));
    std::replace(key.begin(), key.end(), '.', '_');
    std::replace(key.begin(), key.end(), '-', '_');
    return key;
}

bool IsHexDigit(char ch)
{
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

std::uint8_t ParseHexByte(const std::string& value, std::size_t offset)
{
    return static_cast<std::uint8_t>(std::strtoul(value.substr(offset, 2).c_str(), nullptr, 16));
}

}  // namespace

HcommBackend::~HcommBackend()
{
    Finalize();
}

Status HcommBackend::Initialize(const ConnectionManagerConfig& config)
{
    std::lock_guard<std::mutex> lock(mu_);
    config_ = config;
    auto status = LoadEndpointConfigFile();
    if (!status.ok()) { return status; }
    tc_ = static_cast<std::uint8_t>(GetAttrU64("tc", 0));
    sl_ = static_cast<std::uint8_t>(GetAttrU64("sl", 0));
    default_send_size_ = GetAttrU64("send_size", 0);
    default_flag_size_ = GetAttrU64("flag_size", 1);
    default_remote_send_addr_ = ValueToPtr(GetAttrU64("remote_send_addr", 0));
    default_remote_flag_addr_ = ValueToPtr(GetAttrU64("remote_flag_addr", 0));
    initialized_ = true;
    return Status::OK();
}

void HcommBackend::Finalize()
{
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = connections_.begin(); it != connections_.end();) {
        auto handle = it->first;
        ++it;
        (void)DestroyOneConnection(handle);
    }
    initialized_ = false;
}

Status HcommBackend::CreateConnection(const CreateConnectionRequest& request,
                                      std::vector<ConnectionHandle>& connection_handles)
{
    (void)request.timeout_ms;
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialized_) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "hcomm backend is not initialized");
    }
    if (request.qp_num == 0) { return Invalid("qp_num must be greater than 0"); }
    connection_handles.clear();
    active_attrs_ = &request.attrs;
    auto status = CreateConnectionsOnEndpoint(request.local_ip, request.remote_ip, request.port,
                                              request.qp_num, connection_handles);
    active_attrs_ = nullptr;
    return status;
}

std::vector<Status> HcommBackend::DeleteConnections(
    const std::vector<ConnectionHandle>& connection_handles)
{
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Status> statuses;
    statuses.reserve(connection_handles.size());
    for (auto handle : connection_handles) { statuses.emplace_back(DestroyOneConnection(handle)); }
    return statuses;
}

std::vector<Status> HcommBackend::Send(const std::vector<SendIoBatch>& io_batches,
                                       std::uint32_t kernel_count,
                                       std::uint32_t quiet_count)
{
    (void)kernel_count;
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialized_) {
        return SameStatus(io_batches.size(),
                          Status::Error(StatusCode::NOT_INITIALIZED,
                                        "hcomm backend is not initialized"));
    }
    if (quiet_count == 0) {
        return SameStatus(io_batches.size(), Invalid("quiet_count must be greater than 0"));
    }

    std::vector<Status> statuses;
    statuses.reserve(io_batches.size());
    for (std::size_t i = 0; i < io_batches.size(); ++i) {
        auto status = SendOne(io_batches[i], static_cast<std::uint32_t>(i));
        statuses.emplace_back(status);
        if (status.ok() && ((i + 1) % quiet_count == 0)) {
            auto& conn = connections_.at(io_batches[i].connection_handle);
            if (!conn.endpoint) {
                statuses.back() = Status::Error(StatusCode::INTERNAL_ERROR,
                                                "hcomm endpoint is null");
                continue;
            }
            auto ret = HcommProxy::ChannelFenceOnThread(conn.endpoint->GetThreadHandle(),
                                                        conn.channel_handle);
            if (ret != kHcommSuccess) {
                statuses.back() = HcommStatus(ret, "HcommChannelFenceOnThread");
            }
        }
    }
    return statuses;
}

Status HcommBackend::RegisterMemory(ConnectionHandle connection_handle,
                                    const std::vector<RegisterMemoryDesc>& memory_descs,
                                    std::vector<CommMemHandle>& memory_handles)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = connections_.find(connection_handle);
    if (it == connections_.end()) {
        return Status::Error(StatusCode::NOT_FOUND, "connection handle not found");
    }
    memory_handles.clear();
    memory_handles.reserve(memory_descs.size());
    for (const auto& desc : memory_descs) {
        CommMemHandle handle = kInvalidCommMemHandle;
        auto status = RegisterOne(it->second, desc, handle);
        if (!status.ok()) {
            for (auto created : memory_handles) {
                UnregisterMemoryDesc unreg{connection_handle, created};
                (void)UnregisterOne(unreg);
            }
            memory_handles.clear();
            return status;
        }
        memory_handles.emplace_back(handle);
    }
    return Status::OK();
}

std::vector<Status> HcommBackend::UnregisterMemory(
    const std::vector<UnregisterMemoryDesc>& memory_descs)
{
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Status> statuses;
    statuses.reserve(memory_descs.size());
    for (const auto& desc : memory_descs) { statuses.emplace_back(UnregisterOne(desc)); }
    return statuses;
}

Status HcommBackend::BuildEndpoint(const std::string& ip, const std::string& attr_prefix,
                                   EndpointDesc& endpoint) const
{
    auto ret = EndpointDescInit(&endpoint, 1);
    if (ret != kHcommSuccess) { return HcommStatus(ret, "EndpointDescInit"); }

    auto status = FillEndpointProtocol(attr_prefix, endpoint);
    if (!status.ok()) { return status; }
    status = FillEndpointAddress(ip, attr_prefix, endpoint);
    if (!status.ok()) { return status; }
    return FillEndpointLocation(attr_prefix, endpoint);
}

Status HcommBackend::FillEndpointProtocol(const std::string& attr_prefix,
                                          EndpointDesc& endpoint) const
{
    auto protocol = ToLower(GetAttrString(attr_prefix + "_protocol",
                                          GetAttrString("protocol", "roce")));
    if (protocol == "roce") {
        endpoint.protocol = COMM_PROTOCOL_ROCE;
    } else if (protocol == "uboe") {
        endpoint.protocol = COMM_PROTOCOL_UBOE;
    } else if (protocol == "hccs") {
        endpoint.protocol = COMM_PROTOCOL_HCCS;
    } else if (protocol == "ub_ctp" || protocol == "ubc_ctp" || protocol == "ubctp" ||
               protocol == "ub-ctp") {
        endpoint.protocol = COMM_PROTOCOL_UBC_CTP;
    } else if (protocol == "ub_tp" || protocol == "ubc_tp" || protocol == "ubtp" || protocol == "ub-tp" ||
               protocol == "ub") {
        endpoint.protocol = COMM_PROTOCOL_UBC_TP;
    } else {
        return Invalid("unsupported hcomm protocol: " + protocol);
    }
    return Status::OK();
}

Status HcommBackend::FillEndpointAddress(const std::string& ip, const std::string& attr_prefix,
                                         EndpointDesc& endpoint) const
{
    auto comm_id = GetAttrString(attr_prefix + "_comm_id",
                                 GetAttrString(attr_prefix + "_ip", ip));
    if (endpoint.protocol == COMM_PROTOCOL_ROCE || endpoint.protocol == COMM_PROTOCOL_UBOE) {
        if (inet_pton(AF_INET, comm_id.c_str(), &endpoint.commAddr.addr) == 1) {
            endpoint.commAddr.type = COMM_ADDR_TYPE_IP_V4;
            return Status::OK();
        }
        if (inet_pton(AF_INET6, comm_id.c_str(), &endpoint.commAddr.addr6) == 1) {
            endpoint.commAddr.type = COMM_ADDR_TYPE_IP_V6;
            return Status::OK();
        }
        return Invalid("invalid IP endpoint comm_id: " + comm_id);
    }

    if (endpoint.protocol == COMM_PROTOCOL_HCCS) {
        errno = 0;
        char* end = nullptr;
        auto value = std::strtoull(comm_id.c_str(), &end, 0);
        if (errno != 0 || end == comm_id.c_str() || *end != '\0' ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            return Invalid("invalid HCCS endpoint comm_id: " + comm_id);
        }
        endpoint.commAddr.type = COMM_ADDR_TYPE_ID;
        endpoint.commAddr.id = static_cast<std::uint32_t>(value);
        return Status::OK();
    }

    if (comm_id.size() != COMM_ADDR_EID_LEN * 2 ||
        !std::all_of(comm_id.begin(), comm_id.end(), IsHexDigit)) {
        return Invalid("invalid UB endpoint EID comm_id: " + comm_id);
    }
    endpoint.commAddr.type = COMM_ADDR_TYPE_EID;
    for (std::size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        endpoint.commAddr.eid[i] = ParseHexByte(comm_id, i * 2);
    }
    return Status::OK();
}

Status HcommBackend::FillEndpointLocation(const std::string& attr_prefix,
                                          EndpointDesc& endpoint) const
{
    auto placement = ToLower(GetAttrString(attr_prefix + "_placement",
                                           GetAttrString("placement", "host")));
    if (placement == "host") {
        endpoint.loc.locType = ENDPOINT_LOC_TYPE_HOST;
        auto default_host_id = GetAttrU64("host_id", 0);
        endpoint.loc.host.id = static_cast<std::uint32_t>(
            GetAttrU64(attr_prefix + "_host_id", default_host_id));
        return Status::OK();
    }
    if (placement != "device") {
        return Invalid("unsupported hcomm endpoint placement: " + placement);
    }

    endpoint.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    auto default_device_id = GetAttrU64("device_id", 0);
    endpoint.loc.device.devPhyId = static_cast<std::uint32_t>(
        GetAttrU64(attr_prefix + "_phy_device_id",
                   GetAttrU64(attr_prefix + "_dev_phy_id",
                              GetAttrU64(attr_prefix + "_device_id", default_device_id))));
    endpoint.loc.device.superDevId = static_cast<std::uint32_t>(
        GetAttrU64(attr_prefix + "_super_device_id", GetAttrU64("super_device_id", 0)));
    endpoint.loc.device.superPodIdx = static_cast<std::uint32_t>(
        GetAttrU64(attr_prefix + "_super_pod_id", GetAttrU64("super_pod_id", 0)));
    endpoint.loc.device.serverIdx = static_cast<std::uint32_t>(
        GetAttrU64(attr_prefix + "_server_idx", GetAttrU64("server_idx", 0)));
    return Status::OK();
}

Status HcommBackend::LoadEndpointConfigFile()
{
    auto path = GetAttrString("endpoint_config_file", "");
    if (path.empty()) { return Status::OK(); }

    std::ifstream input(path);
    if (!input.is_open()) {
        return Status::Error(StatusCode::NOT_FOUND, "failed to open endpoint config file: " + path);
    }

    std::unordered_map<std::string, std::string> file_attrs;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        auto comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) { line.erase(comment_pos); }
        line = Trim(std::move(line));
        if (line.empty()) { continue; }

        auto sep_pos = line.find('=');
        if (sep_pos == std::string::npos) { sep_pos = line.find(':'); }
        if (sep_pos == std::string::npos) {
            return Invalid("invalid endpoint config line " + std::to_string(line_no) +
                           " in " + path + ": expected key=value");
        }

        auto key = NormalizeConfigKey(line.substr(0, sep_pos));
        auto value = Trim(line.substr(sep_pos + 1));
        if (key.empty()) {
            return Invalid("invalid endpoint config line " + std::to_string(line_no) +
                           " in " + path + ": key is empty");
        }
        file_attrs[key] = value;
    }

    for (auto& item : file_attrs) {
        config_.attrs.emplace(std::move(item.first), std::move(item.second));
    }
    return Status::OK();
}

Status HcommBackend::CreateConnectionsOnEndpoint(
    const std::string& local_ip, const std::string& remote_ip, std::uint32_t port,
    std::uint32_t qp_num, std::vector<ConnectionHandle>& connection_handles)
{
    auto effective_port = port == 0 ? static_cast<std::uint32_t>(GetAttrU64("port", 0)) : port;
    if (effective_port == 0) { return Invalid("port must be greater than 0"); }
    auto effective_tc = static_cast<std::uint8_t>(GetAttrU64("tc", tc_));
    auto effective_sl = static_cast<std::uint8_t>(GetAttrU64("sl", sl_));
    auto send_size = GetAttrU64("send_size", default_send_size_);
    auto flag_size = GetAttrU64("flag_size", default_flag_size_);
    auto remote_send_addr = ValueToPtr(GetAttrU64("remote_send_addr",
                                                 reinterpret_cast<std::uintptr_t>(
                                                     default_remote_send_addr_)));
    auto remote_flag_addr = ValueToPtr(GetAttrU64("remote_flag_addr",
                                                 reinterpret_cast<std::uintptr_t>(
                                                     default_remote_flag_addr_)));

    EndpointDesc local_endpoint{};
    EndpointDesc remote_endpoint{};

    auto status = BuildEndpoint(local_ip, "local", local_endpoint);
    if (!status.ok()) { return status; }
    status = BuildEndpoint(remote_ip, "remote", remote_endpoint);
    if (!status.ok()) { return status; }

    auto endpoint = std::make_shared<HcommEndpoint>(local_endpoint, remote_endpoint);
    status = endpoint->Initialize();
    if (!status.ok()) { return status; }

    std::vector<ChannelHandle> channel_handles;
    status = endpoint->CreateChannels(effective_port, qp_num, effective_tc, effective_sl,
                                      static_cast<std::uint32_t>(GetAttrU64("hccs_qos", 0)),
                                      channel_handles);
    if (!status.ok()) {
        (void)endpoint->Finalize();
        return status;
    }

    connection_handles.clear();
    connection_handles.reserve(qp_num);
    for (auto channel_handle : channel_handles) {
        if (channel_handle == kInvalidConnectionHandle) {
            for (auto created : connection_handles) { (void)DestroyOneConnection(created); }
            (void)endpoint->Finalize();
            connection_handles.clear();
            return Status::Error(StatusCode::INTERNAL_ERROR,
                                 "HcommChannelCreate returned invalid channel handle");
        }
        HcommConnection conn;
        conn.endpoint = endpoint;
        conn.channel_handle = channel_handle;
        conn.send_size = send_size;
        conn.flag_size = flag_size;
        conn.remote_send_addr = remote_send_addr;
        conn.remote_flag_addr = remote_flag_addr;
        conn.owns_channel = true;
        auto emplace_result = connections_.emplace(channel_handle, std::move(conn));
        if (!emplace_result.second) {
            for (auto created : connection_handles) { (void)DestroyOneConnection(created); }
            (void)endpoint->Finalize();
            connection_handles.clear();
            return Status::Error(StatusCode::INTERNAL_ERROR,
                                 "duplicate hcomm channel handle returned");
        }
        connection_handles.emplace_back(channel_handle);
    }
    return Status::OK();
}

Status HcommBackend::DestroyOneConnection(ConnectionHandle handle)
{
    auto it = connections_.find(handle);
    if (it == connections_.end()) {
        return Status::Error(StatusCode::NOT_FOUND, "connection handle not found");
    }

    auto conn = it->second;
    connections_.erase(it);
    Status first = Status::OK();
    if (conn.owns_channel && conn.channel_handle != 0 && conn.endpoint) {
        auto status = conn.endpoint->DestroyChannel(conn.channel_handle);
        if (!status.ok() && first.ok()) { first = status; }
    }
    auto endpoint = std::move(conn.endpoint);
    if (endpoint && endpoint.use_count() == 1) {
        auto status = endpoint->Finalize();
        if (!status.ok() && first.ok()) { first = status; }
    }
    return first;
}

Status HcommBackend::SendOne(const SendIoBatch& io_batch, std::uint32_t index)
{
    auto it = connections_.find(io_batch.connection_handle);
    if (it == connections_.end()) {
        return Status::Error(StatusCode::NOT_FOUND, "connection handle not found");
    }
    auto& conn = it->second;
    if (io_batch.send_buffer == nullptr) { return Invalid("send_buffer is null"); }
    if (io_batch.flag_buffer == nullptr) { return Invalid("flag_buffer is null"); }
    if (conn.send_size == 0) { return Invalid("send_size attr is required for hcomm send"); }
    if (conn.remote_send_addr == nullptr) {
        return Invalid("remote_send_addr attr is required for hcomm send");
    }
    if (conn.remote_flag_addr == nullptr) {
        return Invalid("remote_flag_addr attr is required for hcomm send");
    }

    auto* remote_send =
        static_cast<std::uint8_t*>(conn.remote_send_addr) + index * conn.send_size;
    auto* remote_flag =
        static_cast<std::uint8_t*>(conn.remote_flag_addr) + index * conn.flag_size;
    if (!conn.endpoint) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "hcomm endpoint is null");
    }
    auto ret = HcommProxy::WriteNbiOnThread(conn.endpoint->GetThreadHandle(),
                                            conn.channel_handle, remote_send,
                                            io_batch.send_buffer, conn.send_size);
    if (ret != kHcommSuccess) { return HcommStatus(ret, "HcommWriteNbiOnThread"); }
    ret = HcommProxy::WriteNbiOnThread(conn.endpoint->GetThreadHandle(),
                                       conn.channel_handle, remote_flag,
                                       io_batch.flag_buffer, conn.flag_size);
    return HcommStatus(ret, "HcommWriteNbiOnThread flag");
}

Status HcommBackend::RegisterOne(HcommConnection& connection, const RegisterMemoryDesc& desc,
                                 CommMemHandle& memory_handle)
{
    CommMem mem{};
    mem.type = desc.memory_type == ConnectionMemType::HOST ? COMM_MEM_TYPE_HOST
                                                           : COMM_MEM_TYPE_DEVICE;
    mem.addr = reinterpret_cast<void*>(desc.addr);
    mem.size = desc.size;
    if (!connection.endpoint) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "hcomm endpoint is null");
    }
    return connection.endpoint->RegisterMemory(mem, memory_handle);
}

Status HcommBackend::UnregisterOne(const UnregisterMemoryDesc& desc)
{
    auto conn = connections_.find(desc.connection_handle);
    if (conn == connections_.end()) {
        return Status::Error(StatusCode::NOT_FOUND, "connection handle not found");
    }
    if (!conn->second.endpoint) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "hcomm endpoint is null");
    }
    return conn->second.endpoint->UnregisterMemory(desc.memory_handle);
}

std::uint64_t HcommBackend::GetAttrU64(const std::string& key, std::uint64_t default_value) const
{
    if (active_attrs_ != nullptr) {
        auto active = active_attrs_->find(key);
        if (active != active_attrs_->end() && !active->second.empty()) {
            char* active_end = nullptr;
            auto active_value = std::strtoull(active->second.c_str(), &active_end, 0);
            return (active_end == active->second.c_str()) ? default_value : active_value;
        }
    }
    auto it = config_.attrs.find(key);
    if (it == config_.attrs.end() || it->second.empty()) { return default_value; }
    char* end = nullptr;
    auto value = std::strtoull(it->second.c_str(), &end, 0);
    return (end == it->second.c_str()) ? default_value : value;
}

std::string HcommBackend::GetAttrString(const std::string& key,
                                        const std::string& default_value) const
{
    if (active_attrs_ != nullptr) {
        auto active = active_attrs_->find(key);
        if (active != active_attrs_->end() && !active->second.empty()) { return active->second; }
    }
    auto it = config_.attrs.find(key);
    if (it == config_.attrs.end() || it->second.empty()) { return default_value; }
    return it->second;
}

}  // namespace UC::ASU
