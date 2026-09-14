#pragma once

#include <map>
#include "kv_metrics/default_metric_descriptors.h"
#include "kv_metrics/metrics.h"

namespace kv::metrics {

// Configuration and construction entry point for the self-contained C++
// collector and Prometheus HTTP exporter. This header deliberately stays out
// of metrics.h so users of the facade do not depend on a concrete backend.
struct StandaloneMetricsConfig {
    std::string definitionPath;
    std::string metricPrefix{"ucm:"};
    std::string listenAddress{"127.0.0.1"};
    std::uint16_t port{9108};
    std::string metricsPath{"/metrics"};
    std::uint32_t aggregationIntervalMs{500};
    std::map<std::string, std::string> constantLabels;
};

bool SetUpStandaloneMetrics(StandaloneMetricsConfig config, std::string* error = nullptr);

}  // namespace kv::metrics
