#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "kv_metrics/metric_names.h"

namespace kv::metrics {

enum class MetricType { COUNTER = 0, GAUGE, HISTOGRAM };

struct MetricDescriptor {
    std::string name;
    MetricType type{MetricType::COUNTER};
    std::string documentation;
    std::vector<double> buckets;
};

struct BuiltinMetricUpdate {
    MetricId id;
    double value;
};

struct MetricTimer {
    std::chrono::steady_clock::time_point begin{};
    bool enabled{false};
};

class MetricsBackend {
public:
    virtual ~MetricsBackend() = default;

    virtual bool Start() = 0;
    virtual void Update(std::string_view name, double value) noexcept = 0;
    virtual void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates,
                                    std::size_t count) noexcept = 0;
    virtual void Flush() = 0;
    virtual void Stop() = 0;
    virtual std::string LastError() const = 0;
};

bool Initialize(std::shared_ptr<MetricsBackend> backend, std::string* error = nullptr);
void Shutdown();
void Flush();
bool IsEnabled() noexcept;
MetricTimer StartTimer() noexcept;

void Update(std::string_view name, double value) noexcept;
void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept;

std::vector<MetricDescriptor> DefaultKvMetricDescriptors();

}  // namespace kv::metrics
