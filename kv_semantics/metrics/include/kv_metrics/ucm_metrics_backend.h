#pragma once

#include <memory>
#include "kv_metrics/metrics.h"

namespace kv::metrics {

// Adapts KV metric updates to the process-wide UCM registry initialized by vLLM.
// This adapter never calls UC::Metrics::SetUp or UC::Metrics::CreateStats.
std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter();

}  // namespace kv::metrics
