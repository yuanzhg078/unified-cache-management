#include "asu_metrics/ucm_metrics_backend.h"

#include <exception>
#include <string>
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
    {}

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

    void Add(std::string_view name, double delta) noexcept override { Update(name, delta); }
    void Set(std::string_view name, double value) noexcept override { Update(name, value); }
    void Observe(std::string_view name, double value) noexcept override { Update(name, value); }
    void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates,
                            std::size_t count) noexcept override
    {
        if (updates == nullptr) { return; }
        for (std::size_t index = 0; index < count; ++index) {
            if (ToIndex(updates[index].id) < kBuiltinMetricCount) {
                Update(MetricName(updates[index].id), updates[index].value);
            }
        }
    }
    void Flush() override {}
    void Stop() override {}
    std::string LastError() const override { return error_; }

private:
    void Update(std::string_view name, double value) noexcept
    {
        try {
            UC::Metrics::UpdateStats(std::string{name}, value);
        } catch (...) {
        }
    }

    std::vector<MetricDescriptor> descriptors_;
    std::size_t histogramMaxLength_{10000};
    std::string error_;
};

}  // namespace

std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(
    std::vector<MetricDescriptor> descriptors, std::size_t histogramMaxLength)
{
    return std::make_shared<UcmMetricsBackend>(std::move(descriptors), histogramMaxLength);
}

}  // namespace UC::ASU::Metrics
