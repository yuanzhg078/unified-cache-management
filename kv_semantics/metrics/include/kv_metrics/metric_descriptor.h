#pragma once

#include <string>
#include <vector>
#include "kv_metrics/metrics.h"

namespace kv::metrics {

struct MetricDescriptor {
    std::string name;
    MetricType type{MetricType::COUNTER};
    std::string documentation;
    std::vector<double> buckets;
};

}  // namespace kv::metrics
