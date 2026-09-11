#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "asu_metrics/metrics.h"

namespace UC::ASU::Metrics {

std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(
    std::vector<MetricDescriptor> descriptors = DefaultAsuMetricDescriptors(),
    std::size_t histogramMaxLength = 10000);

}  // namespace UC::ASU::Metrics
