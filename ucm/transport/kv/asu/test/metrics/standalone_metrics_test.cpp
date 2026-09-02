#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "client_task_manager.h"
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

class RecordingMetricsBackend final : public MetricsBackend {
public:
    bool Start() override { return true; }
    void Add(std::string_view, double) noexcept override {}
    void Set(std::string_view, double) noexcept override {}
    void Observe(std::string_view, double) noexcept override {}
    void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept override
    {
        for (std::size_t index = 0; index < count && recordedCount < recordedUpdates.size(); ++index) {
            recordedUpdates[recordedCount++] = updates[index];
        }
    }
    void Flush() override {}
    void Stop() override {}
    std::string LastError() const override { return {}; }

    std::array<BuiltinMetricUpdate, 4> recordedUpdates{};
    std::size_t recordedCount{0};
};

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
    Flush();

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

TEST(StandaloneMetricsTest, AggregatesMetricsWrittenByMultipleThreads)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);

    std::string error;
    ASSERT_TRUE(Initialize(CreateStandaloneMetricsBackend(config), &error)) << error;

    constexpr int kThreadCount = 4;
    constexpr int kUpdatesPerThread = 1000;
    std::vector<std::thread> writers;
    writers.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        writers.emplace_back([] {
            for (int update = 0; update < kUpdatesPerThread; ++update) {
                Add(Names::StoreRequests, 1.0);
                Observe(Names::StoreSubmitDuration, 0.002);
            }
        });
    }
    for (auto& writer : writers) { writer.join(); }
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("ucm:asu_client_store_requests_total 4000"), std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_store_submit_duration_seconds_count 4000"),
              std::string::npos);

    Shutdown();
}

TEST(StandaloneMetricsTest, UpdatesBuiltInMetricsInOneBatch)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);

    std::string error;
    ASSERT_TRUE(Initialize(CreateStandaloneMetricsBackend(config), &error)) << error;

    const BuiltinMetricUpdate updates[] = {
        {MetricId::StoreRequests, 1.0},
        {MetricId::StoreEntries, 8.0},
        {MetricId::StoreSubmitDuration, 0.002},
        {MetricId::ClientTaskPreSendDuration, 0.001},
        {MetricId::ClientTaskSendDuration, 0.001},
        {MetricId::ClientTaskDuration, 0.002},
        {MetricId::TransportTaskPreSendDuration, 0.001},
        {MetricId::TransportTaskSendDuration, 0.001},
        {MetricId::TransportTaskCompletionDuration, 0.001},
    };
    UpdateBuiltinBatch(updates, std::size(updates));
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("ucm:asu_client_store_requests_total 1"), std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_store_entries_total 8"), std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_store_submit_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_task_pre_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_task_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_client_task_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_transport_task_pre_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_transport_task_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:asu_transport_task_completion_duration_seconds_count 1"),
              std::string::npos);

    Shutdown();
    EXPECT_FALSE(IsEnabled());
    EXPECT_FALSE(StartTimer().enabled);
}

TEST(ClientTaskMetricsTest, RecordsApiEntryToCompletionOnce)
{
    auto backend = std::make_shared<RecordingMetricsBackend>();
    std::string error;
    ASSERT_TRUE(Initialize(backend, &error)) << error;

    auto task = std::make_shared<UC::ASU::ClientTask>();
    task->submittedAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(5);
    task->enqueuedAt = std::chrono::steady_clock::now();
    UC::ASU::ClientTaskManager::Finalize(task);
    UC::ASU::ClientTaskManager::Finalize(task);

    const auto duration = std::find_if(
        backend->recordedUpdates.begin(), backend->recordedUpdates.begin() + backend->recordedCount,
        [](const BuiltinMetricUpdate& update) { return update.id == MetricId::ClientTaskDuration; });
    ASSERT_NE(duration, backend->recordedUpdates.begin() + backend->recordedCount);
    EXPECT_GT(duration->value, 0.004);
    EXPECT_EQ(std::count_if(backend->recordedUpdates.begin(),
                            backend->recordedUpdates.begin() + backend->recordedCount,
                            [](const BuiltinMetricUpdate& update) {
                                return update.id == MetricId::ClientTaskDuration;
                            }),
              1);
    EXPECT_TRUE(task->Done());

    Shutdown();
}

TEST(StandaloneMetricsTest, RejectsBuiltinMetricTypeOverride)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);
    config.descriptors.push_back(
        {std::string{MetricName(MetricId::StoreRequests)}, MetricType::GAUGE,
         "invalid override", {}});

    std::string error;
    EXPECT_FALSE(Initialize(CreateStandaloneMetricsBackend(config), &error));
    EXPECT_EQ(error, "built-in metric type cannot be overridden: " +
                         std::string{MetricName(MetricId::StoreRequests)});
    EXPECT_FALSE(IsEnabled());
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
