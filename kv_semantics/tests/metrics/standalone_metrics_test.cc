#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "kv_metrics/metric_names.h"
#include "kv_metrics/metrics.h"
#include "kv_metrics/standalone_metrics_backend.h"
#include "task/task_manager.h"

namespace kv::metrics {
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
    EXPECT_EQ(send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));

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

const char* MetricTypeName(MetricType type)
{
    switch (type) {
        case MetricType::COUNTER: return "counter";
        case MetricType::GAUGE: return "gauge";
        case MetricType::HISTOGRAM: return "histogram";
    }
    return "counter";
}

std::filesystem::path WriteInlineMetricConfig(std::uint16_t port)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("kv-metrics-inline-" + std::to_string(port) + ".yaml");
    std::ofstream output{path};
    EXPECT_TRUE(output.is_open());
    output << "metric_prefix: \"configured:\"\n";
    for (const auto type : {MetricType::COUNTER, MetricType::GAUGE, MetricType::HISTOGRAM}) {
        output << MetricTypeName(type) << ":\n";
        for (const auto& descriptor : DefaultKvMetricDescriptors()) {
            if (descriptor.type != type) { continue; }
            output << "  - {name: \"" << descriptor.name
                   << "\", documentation: \"registered from YAML\"";
            if (type == MetricType::HISTOGRAM) {
                output << ", buckets: [";
                for (std::size_t index = 0; index < descriptor.buckets.size(); ++index) {
                    if (index != 0) { output << ", "; }
                    output << std::setprecision(17) << descriptor.buckets[index];
                }
                output << ']';
            }
            output << "}\n";
        }
    }
    return path;
}

class RecordingKvMetricsBackend final : public KvMetricsBackend {
public:
    void UpdateStats(KvMetricId id, double value) noexcept override
    {
        const KvMetricUpdate update{id, value};
        UpdateStats(&update, 1);
    }
    void UpdateStats(const KvMetricUpdate* updates, std::size_t count) noexcept override
    {
        for (std::size_t index = 0; index < count && recordedCount < recordedUpdates.size();
             ++index) {
            recordedUpdates[recordedCount++] = updates[index];
        }
    }
    std::array<KvMetricUpdate, 4> recordedUpdates{};
    std::size_t recordedCount{0};
};

TEST(MetricsFacadeTest, InstallsOneBackendAndDispatchesUpdates)
{
    auto backend = std::make_shared<RecordingKvMetricsBackend>();
    std::string error;
    ASSERT_TRUE(InstallBackend(backend, &error)) << error;
    UpdateStats(KvMetricId::StoreRequests, 1.0);
    ASSERT_EQ(backend->recordedCount, 1U);
    EXPECT_EQ(backend->recordedUpdates[0].id, KvMetricId::StoreRequests);
    EXPECT_DOUBLE_EQ(backend->recordedUpdates[0].value, 1.0);
    Shutdown();
}

TEST(StandaloneMetricsTest, ExposesCounterGaugeAndHistogramInPrometheusFormat)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);
    config.constantLabels = {
        {"source", "test"}
    };

    std::string error;
    ASSERT_TRUE(SetUpStandaloneMetrics(config, &error)) << error;

    UpdateStats(KvMetricId::StoreRequests, 2.0);
    UpdateStats(KvMetricId::StoreSubmitDuration, 0.002);
    UpdateStats(KvMetricId::ExporterUp, 2.0);
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_requests_total{source=\"test\"} 2"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_submit_duration_seconds_count{source=\"test\"} 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_metrics_exporter_up{source=\"test\"} 2"), std::string::npos);

    Shutdown();
}

TEST(StandaloneMetricsTest, AggregatesMetricsWrittenByMultipleThreads)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);

    std::string error;
    ASSERT_TRUE(SetUpStandaloneMetrics(config, &error)) << error;

    constexpr int kThreadCount = 4;
    constexpr int kUpdatesPerThread = 1000;
    std::vector<std::thread> writers;
    writers.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        writers.emplace_back([] {
            for (int update = 0; update < kUpdatesPerThread; ++update) {
                UpdateStats(KvMetricId::StoreRequests, 1.0);
                UpdateStats(KvMetricId::StoreSubmitDuration, 0.002);
            }
        });
    }
    for (auto& writer : writers) { writer.join(); }
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("ucm:kv_client_store_requests_total 4000"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_submit_duration_seconds_count 4000"),
              std::string::npos);

    Shutdown();
}

TEST(StandaloneMetricsTest, RegistersInlineYamlDefinitionsFromConfiguredSource)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);
    const auto configPath = WriteInlineMetricConfig(config.port);
    config.definitionPath = configPath.string();

    std::string error;
    ASSERT_TRUE(SetUpStandaloneMetrics(config, &error)) << error;
    UpdateStats(KvMetricId::StoreRequests, 3.0);
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("configured:kv_client_store_requests_total 3"), std::string::npos);

    Shutdown();
    std::error_code removeError;
    std::filesystem::remove(configPath, removeError);
    EXPECT_FALSE(removeError);
}

TEST(StandaloneMetricsTest, DoesNotLoseUpdatesDuringConcurrentFlush)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);
    config.aggregationIntervalMs = 1;

    std::string error;
    ASSERT_TRUE(SetUpStandaloneMetrics(config, &error)) << error;

    constexpr int kThreadCount = 4;
    constexpr int kUpdatesPerThread = 2000;
    constexpr int kExpectedUpdates = kThreadCount * kUpdatesPerThread;
    std::atomic<int> finished{0};
    std::vector<std::thread> writers;
    writers.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
        writers.emplace_back([&finished] {
            const KvMetricUpdate updates[] = {
                {KvMetricId::StoreRequests,       1.0  },
                {KvMetricId::StoreSubmitDuration, 0.001},
            };
            for (int update = 0; update < kUpdatesPerThread; ++update) {
                UpdateStats(updates, std::size(updates));
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    while (finished.load(std::memory_order_acquire) != kThreadCount) {
        Flush();
        std::this_thread::yield();
    }
    for (auto& writer : writers) { writer.join(); }
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(
        response.find("ucm:kv_client_store_requests_total " + std::to_string(kExpectedUpdates)),
        std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_submit_duration_seconds_count " +
                            std::to_string(kExpectedUpdates)),
              std::string::npos);

    Shutdown();
}

TEST(StandaloneMetricsTest, UpdatesBuiltInMetricsInOneBatch)
{
    StandaloneMetricsConfig config;
    config.port = FindUnusedLoopbackPort();
    ASSERT_NE(config.port, 0);

    std::string error;
    ASSERT_TRUE(SetUpStandaloneMetrics(config, &error)) << error;

    const KvMetricUpdate updates[] = {
        {KvMetricId::StoreRequests,                   1.0  },
        {KvMetricId::StoreEntries,                    8.0  },
        {KvMetricId::StoreSubmitDuration,             0.002},
        {KvMetricId::ClientTaskPreSendDuration,       0.001},
        {KvMetricId::ClientTaskSendDuration,          0.001},
        {KvMetricId::ClientTaskDuration,              0.002},
        {KvMetricId::ClientTaskQueueWaitNotified,     2.0  },
        {KvMetricId::ClientTaskQueueWaitTimeouts,     6.0  },
        {KvMetricId::ClientTaskQueueNotifies,         3.0  },
        {KvMetricId::TransportTaskPreSendDuration,    0.001},
        {KvMetricId::TransportTaskSendDuration,       0.001},
        {KvMetricId::TransportTaskCompletionDuration, 0.001},
        {KvMetricId::TransportTaskQueueWaitNotified,  4.0  },
        {KvMetricId::TransportTaskQueueWaitTimeouts,  7.0  },
        {KvMetricId::TransportTaskQueueNotifies,      5.0  },
        {KvMetricId::FakeBackendTaskQueueDuration,    0.001},
        {KvMetricId::FakeBackendTaskProcessDuration,  0.001},
    };
    UpdateStats(updates, std::size(updates));
    Flush();

    const auto response = HttpGet(config.port, config.metricsPath);
    EXPECT_NE(response.find("ucm:kv_client_store_requests_total 1"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_entries_total 8"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_store_submit_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_pre_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_send_duration_seconds_count 1"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_duration_seconds_count 1"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_queue_wait_notified_total 2"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_queue_wait_timeout_total 6"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_client_task_queue_notify_total 3"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_pre_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_send_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_completion_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_queue_wait_notified_total 4"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_queue_wait_timeout_total 7"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_transport_task_queue_notify_total 5"), std::string::npos);
    EXPECT_NE(response.find("ucm:kv_fake_backend_task_queue_duration_seconds_count 1"),
              std::string::npos);
    EXPECT_NE(response.find("ucm:kv_fake_backend_task_process_duration_seconds_count 1"),
              std::string::npos);

    Shutdown();
    EXPECT_FALSE(IsEnabled());
    EXPECT_FALSE(StartTimer().enabled);
}

TEST(ClientTaskMetricsTest, RecordsApiEntryToCompletionOnce)
{
    auto backend = std::make_shared<RecordingKvMetricsBackend>();
    std::string error;
    ASSERT_TRUE(InstallBackend(backend, &error)) << error;

    auto task = std::make_shared<::kv::ClientTask>();
    task->submittedAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(5);
    task->enqueuedAt = std::chrono::steady_clock::now();
    ::kv::ClientTaskManager::Finalize(task);
    ::kv::ClientTaskManager::Finalize(task);

    const auto duration = std::find_if(
        backend->recordedUpdates.begin(), backend->recordedUpdates.begin() + backend->recordedCount,
        [](const KvMetricUpdate& update) { return update.id == KvMetricId::ClientTaskDuration; });
    ASSERT_NE(duration, backend->recordedUpdates.begin() + backend->recordedCount);
    EXPECT_GT(duration->value, 0.004);
    EXPECT_EQ(std::count_if(backend->recordedUpdates.begin(),
                            backend->recordedUpdates.begin() + backend->recordedCount,
                            [](const KvMetricUpdate& update) {
                                return update.id == KvMetricId::ClientTaskDuration;
                            }),
              1);
    EXPECT_TRUE(task->Done());

    Shutdown();
}

TEST(StandaloneMetricsTest, RejectsASecondBackendUntilShutdown)
{
    auto first = std::make_shared<RecordingKvMetricsBackend>();
    auto second = std::make_shared<RecordingKvMetricsBackend>();
    std::string error;
    ASSERT_TRUE(InstallBackend(std::move(first), &error));
    EXPECT_FALSE(InstallBackend(std::move(second), &error));
    EXPECT_EQ(error, "metrics backend is already initialized");
    Shutdown();
}

}  // namespace
}  // namespace kv::metrics
