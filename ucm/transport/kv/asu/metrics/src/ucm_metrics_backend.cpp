#include "asu_metrics/ucm_metrics_backend.h"
#include <cmath>
#include <exception>
#include <string>
#include <unordered_map>
#include <utility>
#include "metrics_api.h"

namespace UC::ASU::Metrics {
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
        }
    }

    bool Start() override
    {
        try {
            UC::Metrics::SetUp(histogramMaxLength_);
            for (const auto& descriptor : descriptors_) {
                UC::Metrics::CreateStats(descriptor.name, MetricTypeName(descriptor.type));
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
            if (ToIndex(updates[index].id) < kBuiltinMetricCount) {
                UpdateStats(MetricName(updates[index].id), updates[index].value);
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
        try {
            UC::Metrics::UpdateStats(std::string{name}, value);
        } catch (...) {
        }
    }

    std::vector<MetricDescriptor> descriptors_;
    std::unordered_map<std::string, MetricType> metricTypes_;
    std::size_t histogramMaxLength_{10000};
    std::string error_;
};

}  // namespace

std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(std::vector<MetricDescriptor> descriptors,
                                                        std::size_t histogramMaxLength)
{
    return std::make_shared<UcmMetricsBackend>(std::move(descriptors), histogramMaxLength);
}

}  // namespace UC::ASU::Metrics
