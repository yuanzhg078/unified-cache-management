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
#include <cstdint>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "asu_transport/connection_manager.h"
#include "hcomm_stub.h"

namespace UC::ASU {
namespace {

std::string PtrValue(void* ptr)
{
    return std::to_string(reinterpret_cast<std::uintptr_t>(ptr));
}

ConnectionManagerConfig MakeHcommConfig(void* remote_send, void* remote_flag,
                                        std::uint64_t send_size,
                                        std::uint64_t flag_size)
{
    ConnectionManagerConfig config;
    config.backend_type = ConnectionBackendType::HCOMM;
    config.attrs = {
        {"protocol", "roce"},
        {"tcp_control_plane", "false"},
        {"send_size", std::to_string(send_size)},
        {"flag_size", std::to_string(flag_size)},
        {"remote_send_addr", PtrValue(remote_send)},
        {"remote_flag_addr", PtrValue(remote_flag)},
    };
    return config;
}

CreateConnectionRequest MakeRequest()
{
    CreateConnectionRequest request;
    request.local_ip = "127.0.0.1";
    request.remote_ip = "127.0.0.2";
    request.port = 10001;
    request.qp_num = 2;
    request.timeout_ms = 100;
    return request;
}

}  // namespace

TEST(HcommBackendTest, LocalControlPlaneCreatesChannelsAndSends)
{
    UcmHcommStubReset();
    std::vector<std::uint8_t> remote_send(16, 0);
    std::vector<std::uint8_t> remote_flag(2, 0);
    auto manager = CreateConnectionManager(
        MakeHcommConfig(remote_send.data(), remote_flag.data(), 8, 1));

    auto status = manager->Initialize();
    ASSERT_TRUE(status.ok()) << status.message;

    ConnectionEndpointHandle endpoint_handle = kInvalidConnectionEndpointHandle;
    std::vector<ConnectionHandle> connections;
    status = manager->CreateConnection(MakeRequest(), endpoint_handle, connections);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_NE(endpoint_handle, kInvalidConnectionEndpointHandle);
    ASSERT_EQ(connections.size(), 2U);
    EXPECT_EQ(UcmHcommStubEndpointCreateCount(), 1U);
    EXPECT_EQ(UcmHcommStubChannelCreateCount(), 1U);

    std::vector<std::uint8_t> first_send(8, 7);
    std::vector<std::uint8_t> second_send(8, 9);
    std::uint8_t first_flag = 1;
    std::uint8_t second_flag = 2;
    std::vector<SendIoBatch> batches = {
        SendIoBatch{connections[0], first_send.data(), &first_flag},
        SendIoBatch{connections[1], second_send.data(), &second_flag},
    };

    auto statuses = manager->Send(batches, 0, 2);
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_TRUE(statuses[0].ok()) << statuses[0].message;
    EXPECT_TRUE(statuses[1].ok()) << statuses[1].message;
    EXPECT_EQ(remote_send[0], 7U);
    EXPECT_EQ(remote_send[8], 9U);
    EXPECT_EQ(remote_flag[0], 1U);
    EXPECT_EQ(remote_flag[1], 2U);
    EXPECT_EQ(UcmHcommStubWriteCount(), 4U);
    EXPECT_EQ(UcmHcommStubFenceCount(), 1U);

    statuses = manager->DeleteConnections(connections);
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_TRUE(statuses[0].ok()) << statuses[0].message;
    EXPECT_TRUE(statuses[1].ok()) << statuses[1].message;
    EXPECT_EQ(UcmHcommStubChannelDestroyCount(), 2U);
    EXPECT_EQ(UcmHcommStubEndpointDestroyCount(), 1U);
}

TEST(HcommBackendTest, ReturnsHcommErrorWhenChannelCreateFails)
{
    UcmHcommStubReset();
    UcmHcommStubSetNextChannelCreateResult(123);
    std::vector<std::uint8_t> remote_send(8, 0);
    std::vector<std::uint8_t> remote_flag(1, 0);
    auto manager = CreateConnectionManager(
        MakeHcommConfig(remote_send.data(), remote_flag.data(), 8, 1));
    auto status = manager->Initialize();
    ASSERT_TRUE(status.ok()) << status.message;

    ConnectionEndpointHandle endpoint_handle = kInvalidConnectionEndpointHandle;
    std::vector<ConnectionHandle> connections;
    status = manager->CreateConnection(MakeRequest(), endpoint_handle, connections);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    EXPECT_EQ(endpoint_handle, kInvalidConnectionEndpointHandle);
    EXPECT_TRUE(connections.empty());
    EXPECT_EQ(UcmHcommStubChannelCreateCount(), 1U);
    EXPECT_EQ(UcmHcommStubEndpointDestroyCount(), 1U);
}

}  // namespace UC::ASU

