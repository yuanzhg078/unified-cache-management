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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <csignal>
#include <chrono>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include <utility>
#include "hcomm_proxy.h"

namespace UC::ASU {
namespace {

constexpr HcommResult kHcommSuccess = 0;
constexpr HcommResult kHcommNotSupport = 9;
constexpr std::uint32_t kMagicNumber = 0xA4B3C2D1;
constexpr std::uint32_t kHixlSuccess = 0;
constexpr std::uint64_t kMaxControlBodySize = 4ULL * 1024ULL * 1024ULL;
std::atomic_uint32_t g_next_channel_index{0};

#pragma pack(push, 1)
struct CtrlMsgHeader {
    std::uint32_t magic{0};
    std::uint64_t body_size{0};
};
#pragma pack(pop)

enum class CtrlMsgType : std::int32_t {
    kCreateChannelReq = 1,
    kCreateChannelResp = 2,
    kGetRemoteMemReq = 3,
    kGetRemoteMemResp = 4,
    kMatchEndpointReq = 9,
    kMatchEndpointResp = 10,
};

struct MatchEndpointReq {
    EndpointDesc dst;
};

struct MatchEndpointResp {
    std::uint32_t result{0};
    std::uint64_t dst_ep_handle{0};
};

struct CreateChannelReq {
    EndpointDesc src;
    std::uint64_t dst_ep_handle{0};
    std::uint8_t tc{0};
    std::uint8_t sl{0};
    std::uint32_t channel_index{0};
};

struct CreateChannelResp {
    std::uint32_t result{0};
};

struct GetRemoteMemReq {
    std::uint64_t dst_ep_handle{0};
};

struct RemoteMemDesc {
    CommMem memory{};
    std::string tag;
    std::vector<std::uint8_t> export_desc;
};

class CtrlMsgPlugin {
public:
    static void Initialize();
    static Status Connect(const std::string& ip, std::uint32_t port, int& conn_fd,
                          std::uint32_t timeout_ms);
    static Status Send(int fd, const void* data, std::size_t size);
    static Status Recv(int fd, void* data, std::size_t size, std::uint32_t timeout_ms);
};

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
    CtrlMsgPlugin::Initialize();
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
    for (auto& item : endpoint_sockets_) { CloseFds(item.second); }
    endpoint_sockets_.clear();
    remote_endpoint_handles_.clear();
    imported_remote_mems_.clear();
    endpoints_.clear();
    initialized_ = false;
}

Status HcommBackend::CreateConnection(const CreateConnectionRequest& request,
                                      ConnectionEndpointHandle& endpoint_handle,
                                      std::vector<ConnectionHandle>& connection_handles)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialized_) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "hcomm backend is not initialized");
    }
    if (request.qp_num == 0) { return Invalid("qp_num must be greater than 0"); }
    connection_handles.clear();
    endpoint_handle = kInvalidConnectionEndpointHandle;
    active_attrs_ = &request.attrs;
    auto status =
        UseTcpControlPlane()
            ? CreateClientServerConnection(request.local_ip, request.remote_ip, request.port,
                                           request.qp_num, request.timeout_ms, endpoint_handle,
                                           connection_handles)
            : CreateConnectionsOnEndpoint(request.local_ip, request.remote_ip, request.port,
                                          request.qp_num, endpoint_handle, connection_handles);
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
    std::vector<HcommConnection> connections;
    connections.reserve(io_batches.size());
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) {
            return SameStatus(io_batches.size(),
                              Status::Error(StatusCode::NOT_INITIALIZED,
                                            "hcomm backend is not initialized"));
        }
        if (quiet_count == 0) {
            return SameStatus(io_batches.size(), Invalid("quiet_count must be greater than 0"));
        }
        for (const auto& batch : io_batches) {
            auto it = connections_.find(batch.connection_handle);
            if (it == connections_.end()) {
                return SameStatus(io_batches.size(),
                                  Status::Error(StatusCode::NOT_FOUND,
                                                "connection handle not found"));
            }
            connections.emplace_back(it->second);
        }
    }

    std::vector<Status> statuses;
    statuses.reserve(io_batches.size());
    for (std::size_t i = 0; i < io_batches.size(); ++i) {
        if (!connections[i].io_mu) {
            statuses.emplace_back(Status::Error(StatusCode::INTERNAL_ERROR,
                                                "hcomm connection mutex is null"));
            continue;
        }
        std::lock_guard<std::mutex> io_lock(*connections[i].io_mu);
        auto status = SendOne(connections[i], io_batches[i], static_cast<std::uint32_t>(i));
        statuses.emplace_back(status);
        if (status.ok() && ((i + 1) % quiet_count == 0)) {
            const auto& conn = connections[i];
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

Status HcommBackend::RegisterMemory(ConnectionEndpointHandle endpoint_handle,
                                    const std::vector<RegisterMemoryDesc>& memory_descs,
                                    std::vector<CommMemHandle>& memory_handles)
{
    std::shared_ptr<HcommEndpoint> endpoint;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = endpoints_.find(endpoint_handle);
        if (it == endpoints_.end() || !it->second) {
            return Status::Error(StatusCode::NOT_FOUND, "connection endpoint handle not found");
        }
        endpoint = it->second;
    }
    memory_handles.clear();
    memory_handles.reserve(memory_descs.size());
    for (const auto& desc : memory_descs) {
        CommMemHandle handle = kInvalidCommMemHandle;
        auto status = RegisterOne(*endpoint, desc, handle);
        if (!status.ok()) {
            for (auto created : memory_handles) {
                (void)UnregisterOne(endpoint, created);
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
    std::vector<std::shared_ptr<HcommEndpoint>> endpoints;
    endpoints.reserve(memory_descs.size());
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& desc : memory_descs) {
            auto endpoint = endpoints_.find(desc.endpoint_handle);
            if (endpoint == endpoints_.end() || !endpoint->second) {
                return SameStatus(memory_descs.size(),
                                  Status::Error(StatusCode::NOT_FOUND,
                                                "connection endpoint handle not found"));
            }
            endpoints.emplace_back(endpoint->second);
        }
    }
    std::vector<Status> statuses;
    statuses.reserve(memory_descs.size());
    for (std::size_t i = 0; i < memory_descs.size(); ++i) {
        statuses.emplace_back(UnregisterOne(endpoints[i], memory_descs[i].memory_handle));
    }
    return statuses;
}

Status SystemError(std::string op)
{
    return Status::Error(StatusCode::CONNECTION_ERROR,
                         std::move(op) + " failed, errno=" + std::to_string(errno));
}

void CloseFd(int& fd)
{
    if (fd >= 0) {
        (void)close(fd);
        fd = -1;
    }
}

void CloseFds(std::vector<int>& fds)
{
    for (auto& fd : fds) { CloseFd(fd); }
    fds.clear();
}

void CtrlMsgPlugin::Initialize()
{
    (void)std::signal(SIGPIPE, SIG_IGN);
}

Status SetSocketInt(int fd, int level, int option, int value, const std::string& op)
{
    if (setsockopt(fd, level, option, &value, sizeof(value)) != 0) {
        return SystemError(op);
    }
    return Status::OK();
}

Status SetSocketTimeout(int fd, int option, std::uint32_t timeout_ms, const std::string& op)
{
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    if (setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout)) != 0) {
        return SystemError(op);
    }
    return Status::OK();
}

Status ConfigureTcpControlSocket(int fd, std::uint32_t timeout_ms)
{
    auto status = SetSocketInt(fd, SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt SO_REUSEADDR");
    (void)status;
    status = SetSocketInt(fd, IPPROTO_TCP, TCP_NODELAY, 1, "setsockopt TCP_NODELAY");
    if (!status.ok()) { return status; }
    status = SetSocketTimeout(fd, SO_RCVTIMEO, timeout_ms, "setsockopt SO_RCVTIMEO");
    if (!status.ok()) { return status; }
    return SetSocketTimeout(fd, SO_SNDTIMEO, timeout_ms, "setsockopt SO_SNDTIMEO");
}

Status GetAiFamily(const std::string& ip, int& ai_family)
{
    sockaddr_in ipv4_addr{};
    if (inet_pton(AF_INET, ip.c_str(), &ipv4_addr.sin_addr) == 1) {
        ai_family = AF_INET;
        return Status::OK();
    }

    sockaddr_in6 ipv6_addr{};
    if (inet_pton(AF_INET6, ip.c_str(), &ipv6_addr.sin6_addr) == 1) {
        ai_family = AF_INET6;
        return Status::OK();
    }
    return Status::Error(StatusCode::INVALID_ARGUMENT, "tcp control ip is invalid");
}

Status DoConnect(addrinfo* addr, std::uint32_t timeout_ms, int& fd, int& err_no)
{
    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0) {
        err_no = errno;
        return SystemError("socket");
    }

    auto status = ConfigureTcpControlSocket(fd, timeout_ms);
    if (!status.ok()) {
        err_no = errno;
        CloseFd(fd);
        return status;
    }

    auto ret = connect(fd, addr->ai_addr, addr->ai_addrlen);
    if (ret != 0) {
        err_no = errno;
        auto connect_status = SystemError("connect");
        CloseFd(fd);
        return connect_status;
    }
    return Status::OK();
}

Status CtrlMsgPlugin::Send(int fd, const void* data, std::size_t size)
{
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    auto left = static_cast<ssize_t>(size);
    while (left > 0) {
        auto ret = write(fd, ptr, static_cast<std::size_t>(left));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EINTR) { continue; }
            return SystemError("write");
        }
        if (ret <= 0) {
            return Status::Error(StatusCode::CONNECTION_ERROR, "tcp control peer closed");
        }
        ptr += ret;
        left -= ret;
    }
    return Status::OK();
}

Status CtrlMsgPlugin::Recv(int fd, void* data, std::size_t size,
                           std::uint32_t timeout_ms)
{
    auto* ptr = static_cast<std::uint8_t*>(data);
    auto left = static_cast<ssize_t>(size);
    auto start = std::chrono::steady_clock::now();
    while (left > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        auto remaining = static_cast<std::int64_t>(timeout_ms) - elapsed;
        if (remaining <= 0) {
            return Status::Error(StatusCode::TIMEOUT, "tcp control read timeout");
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        auto poll_ret = poll(&pfd, 1, static_cast<int>(remaining));
        if (poll_ret == 0) {
            return Status::Error(StatusCode::TIMEOUT, "tcp control read poll timeout");
        }
        if (poll_ret < 0) {
            if (errno == EINTR) { continue; }
            return SystemError("poll");
        }

        auto ret = read(fd, ptr, static_cast<std::size_t>(left));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EINTR) { continue; }
            return SystemError("read");
        }
        if (ret <= 0) {
            return Status::Error(StatusCode::CONNECTION_ERROR, "tcp control peer closed");
        }
        ptr += ret;
        left -= ret;
    }
    return Status::OK();
}

template <typename T>
Status SendControlMessage(int fd, CtrlMsgType type, const T& body, std::uint32_t timeout_ms)
{
    CtrlMsgHeader header{};
    header.magic = kMagicNumber;
    header.body_size = static_cast<std::uint64_t>(sizeof(CtrlMsgType) + sizeof(T));
    auto status = CtrlMsgPlugin::Send(fd, &header, sizeof(header));
    if (!status.ok()) { return status; }
    status = CtrlMsgPlugin::Send(fd, &type, sizeof(type));
    if (!status.ok()) { return status; }
    return CtrlMsgPlugin::Send(fd, &body, sizeof(body));
}

Status RecvControlBody(int fd, CtrlMsgType expected, std::vector<std::uint8_t>& body,
                       std::uint32_t timeout_ms)
{
    CtrlMsgHeader header{};
    auto status = CtrlMsgPlugin::Recv(fd, &header, sizeof(header), timeout_ms);
    if (!status.ok()) { return status; }
    if (header.magic != kMagicNumber || header.body_size < sizeof(CtrlMsgType) ||
        header.body_size > kMaxControlBodySize) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "invalid tcp control message header");
    }

    body.resize(static_cast<std::size_t>(header.body_size));
    status = CtrlMsgPlugin::Recv(fd, body.data(), body.size(), timeout_ms);
    if (!status.ok()) { return status; }
    CtrlMsgType actual{};
    std::memcpy(&actual, body.data(), sizeof(actual));
    if (actual != expected) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unexpected tcp control message type");
    }
    return Status::OK();
}

template <typename T>
Status RecvFixedControlMessage(int fd, CtrlMsgType expected, T& message, std::uint32_t timeout_ms)
{
    std::vector<std::uint8_t> body;
    auto status = RecvControlBody(fd, expected, body, timeout_ms);
    if (!status.ok()) { return status; }
    if (body.size() != sizeof(CtrlMsgType) + sizeof(T)) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "unexpected tcp control body size");
    }
    std::memcpy(&message, body.data() + sizeof(CtrlMsgType), sizeof(T));
    return Status::OK();
}

Status CtrlMsgPlugin::Connect(const std::string& ip, std::uint32_t port, int& fd,
                              std::uint32_t timeout_ms)
{
    fd = -1;
    if (ip.empty() || port == 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "tcp control ip or port is invalid");
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    auto status = GetAiFamily(ip, hints.ai_family);
    if (!status.ok()) { return status; }

    addrinfo* result = nullptr;
    auto gai_ret = getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (gai_ret != 0) {
        return Status::Error(StatusCode::INVALID_ARGUMENT,
                             "tcp control getaddrinfo failed: " + std::string(gai_strerror(gai_ret)));
    }

    auto start = std::chrono::high_resolution_clock::now();
    Status last_status = Status::Error(StatusCode::CONNECTION_ERROR, "tcp control connect failed");
    int err_no = 0;
    for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
        last_status = DoConnect(addr, timeout_ms, fd, err_no);
        if (last_status.ok()) { break; }
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (elapsed.count() >= timeout_ms) {
            last_status = Status::Error(StatusCode::TIMEOUT,
                                        "tcp control connect timeout, errno=" +
                                            std::to_string(err_no));
            break;
        }
    }

    freeaddrinfo(result);
    if (!last_status.ok()) { CloseFd(fd); }
    return last_status;
}

Status FindJsonValue(const std::string& json, const std::string& key, std::size_t& value_pos)
{
    auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json field not found: " + key);
    }
    auto colon = json.find(':', key_pos);
    if (colon == std::string::npos) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json field has no value: " + key);
    }
    value_pos = colon + 1;
    while (value_pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[value_pos])) != 0) {
        ++value_pos;
    }
    return Status::OK();
}

Status ParseJsonUint(const std::string& json, const std::string& key, std::uint64_t& value)
{
    std::size_t pos = 0;
    auto status = FindJsonValue(json, key, pos);
    if (!status.ok()) { return status; }
    char* end = nullptr;
    value = std::strtoull(json.c_str() + pos, &end, 0);
    if (end == json.c_str() + pos) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json uint parse failed: " + key);
    }
    return Status::OK();
}

Status ParseJsonString(const std::string& json, const std::string& key, std::string& value)
{
    std::size_t pos = 0;
    auto status = FindJsonValue(json, key, pos);
    if (!status.ok()) { return status; }
    if (pos >= json.size() || json[pos] != '"') {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json string parse failed: " + key);
    }
    ++pos;
    std::ostringstream out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) { ++pos; }
        out << json[pos++];
    }
    if (pos >= json.size()) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json string is unterminated: " + key);
    }
    value = out.str();
    return Status::OK();
}

Status ExtractJsonObject(const std::string& json, std::size_t start, std::string& object,
                         std::size_t& next)
{
    auto begin = json.find('{', start);
    if (begin == std::string::npos) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json object not found");
    }
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = begin; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '"' && (i == 0 || json[i - 1] != '\\')) { in_string = !in_string; }
        if (in_string) { continue; }
        if (ch == '{') { ++depth; }
        if (ch == '}') {
            --depth;
            if (depth == 0) {
                object = json.substr(begin, i - begin + 1);
                next = i + 1;
                return Status::OK();
            }
        }
    }
    return Status::Error(StatusCode::INVALID_ARGUMENT, "json object is unterminated");
}

Status ParseExportDesc(const std::string& json, std::vector<std::uint8_t>& bytes)
{
    std::size_t pos = 0;
    auto status = FindJsonValue(json, "export_desc", pos);
    if (!status.ok()) { return status; }
    if (pos >= json.size() || json[pos] != '[') {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json export_desc is not array");
    }
    ++pos;
    bytes.clear();
    while (pos < json.size()) {
        while (pos < json.size() &&
               std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ']') { return Status::OK(); }
        char* end = nullptr;
        auto value = std::strtoul(json.c_str() + pos, &end, 10);
        if (end == json.c_str() + pos || value > 255) {
            return Status::Error(StatusCode::INVALID_ARGUMENT, "invalid export_desc byte");
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
        pos = static_cast<std::size_t>(end - json.c_str());
        while (pos < json.size() &&
               std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ',') { ++pos; }
    }
    return Status::Error(StatusCode::INVALID_ARGUMENT, "json export_desc is unterminated");
}

Status ParseRemoteMemDescs(const std::string& json, std::vector<RemoteMemDesc>& descs)
{
    std::uint64_t result = 0;
    auto status = ParseJsonUint(json, "result", result);
    if (!status.ok()) { return status; }
    if (result != kHixlSuccess) {
        return Status::Error(StatusCode::IO_ERROR,
                             "remote get mem failed, hixl status=" + std::to_string(result));
    }
    std::size_t pos = 0;
    status = FindJsonValue(json, "mem_descs", pos);
    if (!status.ok()) { return status; }
    if (pos >= json.size() || json[pos] != '[') {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "json mem_descs is not array");
    }
    ++pos;
    descs.clear();
    while (pos < json.size()) {
        while (pos < json.size() &&
               (std::isspace(static_cast<unsigned char>(json[pos])) != 0 || json[pos] == ',')) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ']') { return Status::OK(); }
        std::string item;
        status = ExtractJsonObject(json, pos, item, pos);
        if (!status.ok()) { return status; }
        std::size_t mem_pos = 0;
        status = FindJsonValue(item, "mem", mem_pos);
        if (!status.ok()) { return status; }
        std::string mem_object;
        std::size_t ignored = 0;
        status = ExtractJsonObject(item, mem_pos, mem_object, ignored);
        if (!status.ok()) { return status; }
        std::uint64_t type = 0;
        std::uint64_t addr = 0;
        std::uint64_t size = 0;
        status = ParseJsonUint(mem_object, "type", type);
        if (!status.ok()) { return status; }
        status = ParseJsonUint(mem_object, "addr", addr);
        if (!status.ok()) { return status; }
        status = ParseJsonUint(mem_object, "size", size);
        if (!status.ok()) { return status; }

        RemoteMemDesc desc;
        desc.memory.type = static_cast<CommMemType>(type);
        desc.memory.addr = ValueToPtr(addr);
        desc.memory.size = size;
        (void)ParseJsonString(item, "tag", desc.tag);
        status = ParseExportDesc(item, desc.export_desc);
        if (!status.ok()) { return status; }
        descs.emplace_back(std::move(desc));
    }
    return Status::Error(StatusCode::INVALID_ARGUMENT, "json mem_descs is unterminated");
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
    std::uint32_t qp_num, ConnectionEndpointHandle& endpoint_handle,
    std::vector<ConnectionHandle>& connection_handles)
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
    endpoint_handle = next_endpoint_handle_++;
    if (endpoint_handle == kInvalidConnectionEndpointHandle) { endpoint_handle = next_endpoint_handle_++; }

    std::vector<ChannelHandle> channel_handles;
    status = endpoint->CreateChannels(effective_port, qp_num, effective_tc, effective_sl,
                                      static_cast<std::uint32_t>(GetAttrU64("hccs_qos", 0)),
                                      channel_handles);
    if (!status.ok()) {
        (void)endpoint->Finalize();
        endpoint_handle = kInvalidConnectionEndpointHandle;
        return status;
    }

    connection_handles.clear();
    connection_handles.reserve(qp_num);
    for (auto channel_handle : channel_handles) {
        if (channel_handle == kInvalidConnectionHandle) {
            for (auto created : connection_handles) { (void)DestroyOneConnection(created); }
            (void)endpoint->Finalize();
            connection_handles.clear();
            endpoint_handle = kInvalidConnectionEndpointHandle;
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
            endpoint_handle = kInvalidConnectionEndpointHandle;
            return Status::Error(StatusCode::INTERNAL_ERROR,
                                 "duplicate hcomm channel handle returned");
        }
        connection_handles.emplace_back(channel_handle);
    }
    endpoints_[endpoint_handle] = endpoint;
    return Status::OK();
}

Status HcommBackend::CreateClientServerConnection(
    const std::string& local_ip, const std::string& remote_ip, std::uint32_t port,
    std::uint32_t qp_num, std::uint32_t timeout_ms, ConnectionEndpointHandle& endpoint_handle,
    std::vector<ConnectionHandle>& connection_handles)
{
    auto effective_port = port == 0 ? static_cast<std::uint32_t>(GetAttrU64("port", 0)) : port;
    if (effective_port == 0) { return Invalid("port must be greater than 0"); }
    auto tcp_server_ip = GetAttrString("tcp_server_ip", GetAttrString("server_ip", remote_ip));
    auto channel_port = static_cast<std::uint32_t>(GetAttrU64("channel_port", 0));
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

    std::vector<int> control_fds;
    int socket_fd = -1;
    status = CtrlMsgPlugin::Connect(tcp_server_ip, effective_port, socket_fd, timeout_ms);
    if (!status.ok()) {
        (void)endpoint->Finalize();
        return status;
    }
    control_fds.emplace_back(socket_fd);

    MatchEndpointReq match_req{};
    match_req.dst = remote_endpoint;
    status = SendControlMessage(socket_fd, CtrlMsgType::kMatchEndpointReq, match_req, timeout_ms);
    if (!status.ok()) {
        CloseFds(control_fds);
        (void)endpoint->Finalize();
        return status;
    }

    MatchEndpointResp match_resp{};
    status = RecvFixedControlMessage(socket_fd, CtrlMsgType::kMatchEndpointResp, match_resp,
                                     timeout_ms);
    if (!status.ok() || match_resp.result != kHixlSuccess || match_resp.dst_ep_handle == 0) {
        CloseFds(control_fds);
        (void)endpoint->Finalize();
        if (status.ok()) {
            status = Status::Error(StatusCode::CONNECTION_ERROR,
                                   "remote endpoint match failed, hixl status=" +
                                       std::to_string(match_resp.result));
        }
        return status;
    }

    std::vector<ImportedRemoteMemory> imported;
    status = ImportRemoteMemories(socket_fd, endpoint, match_resp.dst_ep_handle, timeout_ms,
                                  imported);
    if (!status.ok()) {
        CloseFds(control_fds);
        (void)endpoint->Finalize();
        return status;
    }

    const auto remote_send_tag = GetAttrString("remote_send_tag", "send");
    const auto remote_flag_tag = GetAttrString("remote_flag_tag", "flag");
    for (const auto& item : imported) {
        if (remote_send_addr == nullptr && item.tag == remote_send_tag) {
            remote_send_addr = item.memory.addr;
            send_size = item.memory.size;
        }
        if (remote_flag_addr == nullptr && item.tag == remote_flag_tag) {
            remote_flag_addr = item.memory.addr;
            flag_size = item.memory.size;
        }
    }
    if (remote_send_addr == nullptr && !imported.empty()) {
        remote_send_addr = imported[0].memory.addr;
        send_size = imported[0].memory.size;
    }
    if (remote_flag_addr == nullptr && imported.size() > 1) {
        remote_flag_addr = imported[1].memory.addr;
        flag_size = imported[1].memory.size;
    }

    endpoint_handle = next_endpoint_handle_++;
    if (endpoint_handle == kInvalidConnectionEndpointHandle) { endpoint_handle = next_endpoint_handle_++; }

    connection_handles.clear();
    connection_handles.reserve(qp_num);
    for (std::uint32_t i = 0; i < qp_num; ++i) {
        if (i > 0) {
            socket_fd = -1;
            status = CtrlMsgPlugin::Connect(tcp_server_ip, effective_port, socket_fd,
                                            timeout_ms);
            if (!status.ok()) { break; }
            control_fds.emplace_back(socket_fd);

            status = SendControlMessage(socket_fd, CtrlMsgType::kMatchEndpointReq, match_req,
                                        timeout_ms);
            if (!status.ok()) { break; }

            MatchEndpointResp per_socket_match_resp{};
            status = RecvFixedControlMessage(socket_fd, CtrlMsgType::kMatchEndpointResp,
                                             per_socket_match_resp, timeout_ms);
            if (!status.ok() || per_socket_match_resp.result != kHixlSuccess ||
                per_socket_match_resp.dst_ep_handle == 0) {
                if (status.ok()) {
                    status = Status::Error(StatusCode::CONNECTION_ERROR,
                                           "remote endpoint match failed, hixl status=" +
                                               std::to_string(per_socket_match_resp.result));
                }
                break;
            }
            match_resp.dst_ep_handle = per_socket_match_resp.dst_ep_handle;
        } else {
            socket_fd = control_fds.back();
        }

        const auto channel_index = g_next_channel_index.fetch_add(1U, std::memory_order_relaxed);
        CreateChannelReq create_req{};
        create_req.src = local_endpoint;
        create_req.dst_ep_handle = match_resp.dst_ep_handle;
        create_req.tc = effective_tc;
        create_req.sl = effective_sl;
        create_req.channel_index = channel_index;
        status = SendControlMessage(socket_fd, CtrlMsgType::kCreateChannelReq, create_req,
                                    timeout_ms);
        if (!status.ok()) { break; }

        ChannelHandle channel_handle = 0;
        status = endpoint->CreateChannel(channel_port, effective_tc, effective_sl,
                                         static_cast<std::uint32_t>(GetAttrU64("hccs_qos", 0)),
                                         channel_index, channel_handle);
        if (!status.ok()) { break; }

        CreateChannelResp create_resp{};
        status = RecvFixedControlMessage(socket_fd, CtrlMsgType::kCreateChannelResp, create_resp,
                                         timeout_ms);
        if (!status.ok() || create_resp.result != kHixlSuccess) {
            (void)endpoint->DestroyChannel(channel_handle);
            if (status.ok()) {
                status = Status::Error(StatusCode::CONNECTION_ERROR,
                                       "remote create channel failed, hixl status=" +
                                           std::to_string(create_resp.result));
            }
            break;
        }

        HcommConnection conn;
        conn.endpoint = endpoint;
        conn.channel_handle = channel_handle;
        conn.send_size = send_size;
        conn.flag_size = flag_size;
        conn.remote_send_addr = remote_send_addr;
        conn.remote_flag_addr = remote_flag_addr;
        conn.owns_channel = true;
        conn.control_fd = socket_fd;
        auto emplace_result = connections_.emplace(channel_handle, std::move(conn));
        if (!emplace_result.second) {
            (void)endpoint->DestroyChannel(channel_handle);
            status = Status::Error(StatusCode::INTERNAL_ERROR,
                                   "duplicate hcomm channel handle returned");
            break;
        }
        connection_handles.emplace_back(channel_handle);
    }

    if (!status.ok()) {
        for (auto created : connection_handles) {
            connections_.erase(created);
            (void)endpoint->DestroyChannel(created);
        }
        CloseFds(control_fds);
        for (auto& item : imported) {
            (void)endpoint->UnimportMemory(item.export_desc.data(),
                                           static_cast<std::uint32_t>(item.export_desc.size()));
        }
        (void)endpoint->Finalize();
        connection_handles.clear();
        endpoint_handle = kInvalidConnectionEndpointHandle;
        return status;
    }

    endpoints_[endpoint_handle] = endpoint;
    endpoint_sockets_[endpoint_handle] = std::move(control_fds);
    remote_endpoint_handles_[endpoint_handle] = match_resp.dst_ep_handle;
    imported_remote_mems_[endpoint_handle] = std::move(imported);
    return Status::OK();
}

Status HcommBackend::ImportRemoteMemories(int socket_fd,
                                          const std::shared_ptr<HcommEndpoint>& endpoint,
                                          std::uint64_t remote_endpoint_handle,
                                          std::uint32_t timeout_ms,
                                          std::vector<ImportedRemoteMemory>& imported)
{
    if (!endpoint) {
        return Status::Error(StatusCode::INTERNAL_ERROR, "hcomm endpoint is null");
    }
    GetRemoteMemReq request{};
    request.dst_ep_handle = remote_endpoint_handle;
    auto status = SendControlMessage(socket_fd, CtrlMsgType::kGetRemoteMemReq, request, timeout_ms);
    if (!status.ok()) { return status; }

    std::vector<std::uint8_t> body;
    status = RecvControlBody(socket_fd, CtrlMsgType::kGetRemoteMemResp, body, timeout_ms);
    if (!status.ok()) { return status; }
    const auto* json_ptr = reinterpret_cast<const char*>(body.data() + sizeof(CtrlMsgType));
    const auto json_len = body.size() - sizeof(CtrlMsgType);
    std::vector<RemoteMemDesc> remote_descs;
    status = ParseRemoteMemDescs(std::string(json_ptr, json_len), remote_descs);
    if (!status.ok()) { return status; }

    auto close_imported_bufs = [&endpoint](std::vector<ImportedRemoteMemory>& bufs) {
        if (!endpoint) { return; }
        for (auto& item : bufs) {
            (void)endpoint->UnimportMemory(
                item.export_desc.data(), static_cast<std::uint32_t>(item.export_desc.size()));
        }
        bufs.clear();
    };

    auto validate_export_desc_list = [](const std::vector<RemoteMemDesc>& desc_list) -> Status {
        for (const auto& desc : desc_list) {
            if (desc.export_desc.empty()) {
                return Status::Error(StatusCode::INVALID_ARGUMENT,
                                     "remote memory export_desc is empty");
            }
        }
        return Status::OK();
    };

    auto import_one_desc = [&endpoint](const RemoteMemDesc& desc,
                                       ImportedRemoteMemory& item) -> Status {
        CommMem imported_mem{};
        auto import_status = endpoint->ImportMemory(
            desc.export_desc.data(), static_cast<std::uint32_t>(desc.export_desc.size()),
            imported_mem);
        if (!import_status.ok()) { return import_status; }
        item.tag = desc.tag;
        item.memory = desc.memory;
        item.export_desc = desc.export_desc;
        return Status::OK();
    };

    auto import_all_descs = [&](const std::vector<RemoteMemDesc>& desc_list,
                                std::vector<ImportedRemoteMemory>& output) -> Status {
        output.clear();
        output.reserve(desc_list.size());
        for (const auto& desc : desc_list) {
            ImportedRemoteMemory item;
            auto import_status = import_one_desc(desc, item);
            if (!import_status.ok()) {
                close_imported_bufs(output);
                return import_status;
            }
            output.emplace_back(std::move(item));
        }
        return Status::OK();
    };

    close_imported_bufs(imported);
    imported.clear();
    if (remote_descs.empty()) { return Status::OK(); }

    status = validate_export_desc_list(remote_descs);
    if (!status.ok()) { return status; }
    return import_all_descs(remote_descs, imported);
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
    if (conn.control_fd >= 0) {
        for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end(); ++ep_it) {
            if (ep_it->second != endpoint) { continue; }
            auto socket_it = endpoint_sockets_.find(ep_it->first);
            if (socket_it == endpoint_sockets_.end()) { break; }
            auto& sockets = socket_it->second;
            auto fd_it = std::find(sockets.begin(), sockets.end(), conn.control_fd);
            if (fd_it != sockets.end()) {
                CloseFd(*fd_it);
                sockets.erase(fd_it);
            }
            break;
        }
    }
    bool endpoint_in_use = false;
    for (const auto& item : connections_) {
        if (item.second.endpoint == endpoint) {
            endpoint_in_use = true;
            break;
        }
    }
    if (endpoint && !endpoint_in_use) {
        for (auto ep_it = endpoints_.begin(); ep_it != endpoints_.end();) {
            if (ep_it->second == endpoint) {
                auto imported_it = imported_remote_mems_.find(ep_it->first);
                if (imported_it != imported_remote_mems_.end()) {
                    for (auto& item : imported_it->second) {
                        (void)endpoint->UnimportMemory(
                            item.export_desc.data(),
                            static_cast<std::uint32_t>(item.export_desc.size()));
                    }
                    imported_remote_mems_.erase(imported_it);
                }
                auto socket_it = endpoint_sockets_.find(ep_it->first);
                if (socket_it != endpoint_sockets_.end()) {
                    CloseFds(socket_it->second);
                    endpoint_sockets_.erase(socket_it);
                }
                remote_endpoint_handles_.erase(ep_it->first);
                ep_it = endpoints_.erase(ep_it);
            } else {
                ++ep_it;
            }
        }
        auto status = endpoint->Finalize();
        if (!status.ok() && first.ok()) { first = status; }
    }
    return first;
}

Status HcommBackend::SendOne(const HcommConnection& conn, const SendIoBatch& io_batch,
                             std::uint32_t index)
{
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

Status HcommBackend::RegisterOne(HcommEndpoint& endpoint, const RegisterMemoryDesc& desc,
                                 CommMemHandle& memory_handle)
{
    CommMem mem{};
    mem.type = desc.memory_type == ConnectionMemType::HOST ? COMM_MEM_TYPE_HOST
                                                           : COMM_MEM_TYPE_DEVICE;
    mem.addr = reinterpret_cast<void*>(desc.addr);
    mem.size = desc.size;
    return endpoint.RegisterMemory(mem, memory_handle);
}

Status HcommBackend::UnregisterOne(const std::shared_ptr<HcommEndpoint>& endpoint,
                                   CommMemHandle memory_handle)
{
    if (!endpoint) { return Status::Error(StatusCode::INTERNAL_ERROR, "hcomm endpoint is null"); }
    return endpoint->UnregisterMemory(memory_handle);
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

bool HcommBackend::UseTcpControlPlane() const
{
    auto value = ToLower(GetAttrString("tcp_control_plane", "true"));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

}  // namespace UC::ASU
