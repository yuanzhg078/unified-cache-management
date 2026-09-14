#include "kv_metrics/ucm_metrics_backend.h"
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include "metrics_api.h"

namespace kv::metrics {
namespace {

bool IsCounter(KvMetricId id) noexcept
{
    switch (id) {
#define KV_IS_COUNTER_CASE(metricId, name, type, documentation) \
    case KvMetricId::metricId: return MetricType::type == MetricType::COUNTER;
        KV_BUILTIN_METRIC_LIST(KV_IS_COUNTER_CASE)
#undef KV_IS_COUNTER_CASE
        case KvMetricId::COUNT: return false;
    }
    return false;
}

class UcmKvMetricsAdapter final : public KvMetricsBackend {
public:
    UcmKvMetricsAdapter()
    {
        for (std::size_t index = 0; index < kBuiltinMetricCount; ++index) {
            const auto id = static_cast<KvMetricId>(index);
            metrics_[index] =
                std::make_unique<UC::Metrics::CachedMetric>(std::string{MetricName(id)});
        }
    }

    void UpdateStats(KvMetricId id, double value) noexcept override
    {
        const auto index = ToIndex(id);
        if (index >= kBuiltinMetricCount || !std::isfinite(value) ||
            (IsCounter(id) && value < 0.0)) {
            return;
        }
        try {
            UC::Metrics::UpdateStats(*metrics_[index], value);
        } catch (...) {
        }
    }

    void UpdateStats(const KvMetricUpdate* updates, std::size_t count) noexcept override
    {
        if (updates == nullptr) { return; }
        for (std::size_t index = 0; index < count; ++index) {
            UpdateStats(updates[index].id, updates[index].value);
        }
    }

private:
    std::array<std::unique_ptr<UC::Metrics::CachedMetric>, kBuiltinMetricCount> metrics_{};
};

}  // namespace

std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter()
{
    return std::make_shared<UcmKvMetricsAdapter>();
}

}  // namespace kv::metrics
