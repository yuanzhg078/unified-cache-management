#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "kv_metrics/metric_names.h"

namespace kv::metrics {

enum class MetricType { COUNTER = 0, GAUGE, HISTOGRAM };

struct KvMetricUpdate {
    KvMetricId id;
    double value;
};

struct MetricTimer {
    std::chrono::steady_clock::time_point begin{};
    bool enabled{false};
};

class KvMetricsBackend {
public:
    virtual ~KvMetricsBackend() = default;

    virtual void UpdateStats(KvMetricId id, double value) noexcept = 0;
    virtual void UpdateStats(const KvMetricUpdate* updates, std::size_t count) noexcept = 0;
    virtual void Flush() {}
    virtual void Stop() {}
};

bool InstallBackend(std::shared_ptr<KvMetricsBackend> backend, std::string* error = nullptr);
void Shutdown();
void Flush();
bool IsEnabled() noexcept;
MetricTimer StartTimer() noexcept;

void UpdateStats(KvMetricId id, double value) noexcept;
void UpdateStats(const KvMetricUpdate* updates, std::size_t count) noexcept;

}  // namespace kv::metrics
