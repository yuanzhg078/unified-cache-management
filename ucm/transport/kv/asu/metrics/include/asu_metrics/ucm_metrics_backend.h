#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "asu_metrics/metrics.h"

namespace UC::ASU::Metrics {

// This factory is available only when BUILD_ASU_UCM_METRICS_ADAPTER=ON.
std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(
    std::vector<MetricDescriptor> descriptors = DefaultAsuMetricDescriptors(),
    std::size_t histogramMaxLength = 10000);

}  // namespace UC::ASU::Metrics
