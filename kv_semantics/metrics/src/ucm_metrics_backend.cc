#include "kv_metrics/ucm_metrics_backend.h"
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include "metrics_api.h"

namespace kv::metrics {
namespace {

const char* MetricTypeName(MetricType type)
{
    switch (type) {
        case MetricType::COUNTER: return "counter";
        case MetricType::GAUGE: return "gauge";
        case MetricType::HISTOGRAM: return "histogram";
        default: return "";
    }
}

class UcmMetricsBackend final : public MetricsBackend {
public:
    UcmMetricsBackend(std::vector<MetricDescriptor> descriptors, std::size_t histogramMaxLength)
        : descriptors_(std::move(descriptors)), histogramMaxLength_(histogramMaxLength)
    {
        for (const auto& descriptor : descriptors_) {
            metricTypes_.emplace(descriptor.name, descriptor.type);
            cachedMetrics_.emplace(descriptor.name,
                                   std::make_unique<UC::Metrics::CachedMetric>(descriptor.name));
        }
        for (std::size_t index = 0; index < kBuiltinMetricCount; ++index) {
            const auto iter =
                cachedMetrics_.find(std::string{MetricName(static_cast<MetricId>(index))});
            if (iter != cachedMetrics_.end()) { builtinMetrics_[index] = iter->second.get(); }
        }
    }

    bool Start() override
    {
        try {
            UC::Metrics::SetUp(histogramMaxLength_);
            for (const auto& descriptor : descriptors_) {
                UC::Metrics::CreateStats(descriptor.name, MetricTypeName(descriptor.type),
                                         descriptor.buckets);
            }
            return true;
        } catch (const std::exception& error) {
            error_ = error.what();
            return false;
        }
    }

    void Update(std::string_view name, double value) noexcept override { UpdateStats(name, value); }
    void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept override
    {
        if (updates == nullptr) { return; }
        for (std::size_t index = 0; index < count; ++index) {
            const auto metricIndex = ToIndex(updates[index].id);
            if (metricIndex >= kBuiltinMetricCount || builtinMetrics_[metricIndex] == nullptr ||
                !std::isfinite(updates[index].value)) {
                continue;
            }
            const auto type = metricTypes_.find(builtinMetrics_[metricIndex]->name);
            if (type != metricTypes_.end() && type->second == MetricType::COUNTER &&
                updates[index].value < 0.0) {
                continue;
            }
            try {
                UC::Metrics::UpdateStats(*builtinMetrics_[metricIndex], updates[index].value);
            } catch (...) {
            }
        }
    }
    void Flush() override {}
    void Stop() override {}
    std::string LastError() const override { return error_; }

private:
    void UpdateStats(std::string_view name, double value) noexcept
    {
        if (!std::isfinite(value)) { return; }
        const auto iter = metricTypes_.find(std::string{name});
        if (iter == metricTypes_.end() || (iter->second == MetricType::COUNTER && value < 0.0)) {
            return;
        }
        const auto cached = cachedMetrics_.find(std::string{name});
        if (cached == cachedMetrics_.end()) { return; }
        try {
            UC::Metrics::UpdateStats(*cached->second, value);
        } catch (...) {
        }
    }

    std::vector<MetricDescriptor> descriptors_;
    std::unordered_map<std::string, MetricType> metricTypes_;
    std::unordered_map<std::string, std::unique_ptr<UC::Metrics::CachedMetric>> cachedMetrics_;
    std::array<UC::Metrics::CachedMetric*, kBuiltinMetricCount> builtinMetrics_{};
    std::size_t histogramMaxLength_{10000};
    std::string error_;
};

}  // namespace

std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(std::vector<MetricDescriptor> descriptors,
                                                        std::size_t histogramMaxLength)
{
    return std::make_shared<UcmMetricsBackend>(std::move(descriptors), histogramMaxLength);
}

}  // namespace kv::metrics
