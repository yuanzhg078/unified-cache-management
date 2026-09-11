#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "kv_metrics/metrics.h"

namespace kv::metrics {

std::shared_ptr<MetricsBackend> CreateUcmMetricsBackend(
    std::vector<MetricDescriptor> descriptors = DefaultKvMetricDescriptors(),
    std::size_t histogramMaxLength = 10000);

}  // namespace kv::metrics
