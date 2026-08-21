#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "asu_metrics/metric_names.h"
#include "asu_metrics/metrics.h"

namespace UC::ASU::Metrics {
namespace {

std::uint16_t FindUnusedLoopbackPort()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    if (fd < 0) { return 0; }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    socklen_t length = sizeof(address);
    EXPECT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    const auto port = ntohs(address.sin_port);
    close(fd);
    return port;
}

std::string HttpGet(std::uint16_t port, const std::string& path)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    if (fd < 0) { return {}; }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    EXPECT_EQ(connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    const auto request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT_EQ(send(fd, request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));

    std::string response;
    char buffer[4096];
    while (true) {
        const auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) { break; }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return response;
}

TEST(StandaloneMetricsTest, ExposesCounterGaugeAndHistogramInPrometheusFormat)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);
    config.constantLabels = {{"source", "test"}};

    std::string error;
    ASSERT_TRUE(Initialize(CreateStandaloneMetricsBackend(config), &error)) << error;

    Add(Names::StoreRequests, 2.0);
    Observe(Names::StoreSubmitDuration, 0.002);

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find(
                  "ucm:asu_client_store_requests_total{source=\"test\"} 2"),
              std::string::npos);
    EXPECT_NE(response.find(
                  "ucm:asu_client_store_submit_duration_seconds_count{source=\"test\"} 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_metrics_exporter_up{source=\"test\"} 1"),
              std::string::npos);

    Shutdown();
}

TEST(StandaloneMetricsTest, RejectsASecondBackendUntilShutdown)
{
    auto first = CreateNoopMetricsBackend();
    auto second = CreateNoopMetricsBackend();
    std::string error;
    ASSERT_TRUE(Initialize(std::move(first), &error));
    EXPECT_FALSE(Initialize(std::move(second), &error));
    EXPECT_EQ(error, "metrics backend is already initialized");
    Shutdown();
}

}  // namespace
}  // namespace UC::ASU::Metrics
