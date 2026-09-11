#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kv::metrics {

// One definition table generates the built-in IDs, names, and descriptors.
#define KV_BUILTIN_METRIC_LIST(X)                                                                 \
    X(QueryRequests, "kv_client_query_requests_total", COUNTER,                                   \
      "Total KV client query submissions")                                                        \
    X(QueryEntries, "kv_client_query_entries_total", COUNTER,                                     \
      "Total keys submitted to KV client query")                                                  \
    X(QueryErrors, "kv_client_query_errors_total", COUNTER,                                       \
      "Total failed KV client query submissions")                                                 \
    X(QuerySubmitDuration, "kv_client_query_submit_duration_seconds", HISTOGRAM,                  \
      "KV client query submission duration in seconds")                                           \
    X(LoadRequests, "kv_client_load_requests_total", COUNTER,                                     \
      "Total KV client load submissions")                                                         \
    X(LoadEntries, "kv_client_load_entries_total", COUNTER,                                       \
      "Total entries submitted to KV client load")                                                \
    X(LoadErrors, "kv_client_load_errors_total", COUNTER,                                         \
      "Total failed KV client load submissions")                                                  \
    X(LoadSubmitDuration, "kv_client_load_submit_duration_seconds", HISTOGRAM,                    \
      "KV client load submission duration in seconds")                                            \
    X(StoreRequests, "kv_client_store_requests_total", COUNTER,                                   \
      "Total KV client store submissions")                                                        \
    X(StoreEntries, "kv_client_store_entries_total", COUNTER,                                     \
      "Total entries submitted to KV client store")                                               \
    X(StoreErrors, "kv_client_store_errors_total", COUNTER,                                       \
      "Total failed KV client store submissions")                                                 \
    X(StoreSubmitDuration, "kv_client_store_submit_duration_seconds", HISTOGRAM,                  \
      "KV client store submission duration in seconds")                                           \
    X(BatchLoadRequests, "kv_client_batch_load_requests_total", COUNTER,                          \
      "Total KV client batch-load submissions")                                                   \
    X(BatchLoadEntries, "kv_client_batch_load_entries_total", COUNTER,                            \
      "Total entries submitted to KV client batch-load")                                          \
    X(BatchLoadErrors, "kv_client_batch_load_errors_total", COUNTER,                              \
      "Total failed KV client batch-load submissions")                                            \
    X(BatchLoadSubmitDuration, "kv_client_batch_load_submit_duration_seconds", HISTOGRAM,         \
      "KV client batch-load submission duration in seconds")                                      \
    X(BatchStoreRequests, "kv_client_batch_store_requests_total", COUNTER,                        \
      "Total KV client batch-store submissions")                                                  \
    X(BatchStoreEntries, "kv_client_batch_store_entries_total", COUNTER,                          \
      "Total entries submitted to KV client batch-store")                                         \
    X(BatchStoreErrors, "kv_client_batch_store_errors_total", COUNTER,                            \
      "Total failed KV client batch-store submissions")                                           \
    X(BatchStoreSubmitDuration, "kv_client_batch_store_submit_duration_seconds", HISTOGRAM,       \
      "KV client batch-store submission duration in seconds")                                     \
    X(DeleteRequests, "kv_client_delete_requests_total", COUNTER,                                 \
      "Total KV client delete submissions")                                                       \
    X(DeleteEntries, "kv_client_delete_entries_total", COUNTER,                                   \
      "Total keys submitted to KV client delete")                                                 \
    X(DeleteErrors, "kv_client_delete_errors_total", COUNTER,                                     \
      "Total failed KV client delete submissions")                                                \
    X(DeleteSubmitDuration, "kv_client_delete_submit_duration_seconds", HISTOGRAM,                \
      "KV client delete submission duration in seconds")                                          \
    X(WaitRequests, "kv_client_wait_requests_total", COUNTER, "Total KV client wait calls")      \
    X(WaitErrors, "kv_client_wait_errors_total", COUNTER, "Total failed KV client wait calls")   \
    X(WaitDuration, "kv_client_wait_duration_seconds", HISTOGRAM,                                 \
      "KV client wait duration in seconds")                                                       \
    X(ClientTaskEnqueueDuration, "kv_client_task_enqueue_duration_seconds", HISTOGRAM,            \
      "KV client task duration from API entry until client queue enqueue")                        \
    X(ClientTaskQueueDuration, "kv_client_task_queue_duration_seconds", HISTOGRAM,                \
      "KV client task queue wait duration")                                                       \
    X(ClientTaskProcessDuration, "kv_client_task_process_duration_seconds", HISTOGRAM,            \
      "KV client worker processing duration until all transport tasks are submitted")             \
    X(ClientTaskPreSendDuration, "kv_client_task_pre_send_duration_seconds", HISTOGRAM,           \
      "KV client task duration from enqueue until all transport tasks reach provider Send")       \
    X(ClientTaskSendDuration, "kv_client_task_send_duration_seconds", HISTOGRAM,                  \
      "KV client task duration from enqueue until all transport Send calls return")               \
    X(ClientTaskDuration, "kv_client_task_duration_seconds", HISTOGRAM,                           \
      "KV client task end-to-end duration from API entry to completion")                          \
    X(ClientTaskQueueWaitNotified, "kv_client_task_queue_wait_notified_total", COUNTER,           \
      "Total KV client task queue waits completed by notification")                               \
    X(ClientTaskQueueWaitTimeouts, "kv_client_task_queue_wait_timeout_total", COUNTER,            \
      "Total KV client task queue waits completed by timeout")                                    \
    X(ClientTaskQueueNotifies, "kv_client_task_queue_notify_total", COUNTER,                      \
      "Total KV client task queue condition-variable notifications")                              \
    X(TransportTaskPreSendDuration, "kv_transport_task_pre_send_duration_seconds", HISTOGRAM,     \
      "KV transport task duration from client dispatch until immediately before Send")            \
    X(TransportTaskQueueDuration, "kv_transport_task_queue_duration_seconds", HISTOGRAM,          \
      "KV transport task queue wait duration")                                                    \
    X(TransportTaskProcessDuration, "kv_transport_task_process_duration_seconds", HISTOGRAM,      \
      "KV transport executor processing duration until immediately before Send")                  \
    X(TransportTaskSendDuration, "kv_transport_task_send_duration_seconds", HISTOGRAM,            \
      "KV transport task duration from client dispatch until Send returns")                       \
    X(TransportTaskCompletionDuration, "kv_transport_task_completion_duration_seconds",           \
      HISTOGRAM, "KV transport task duration from Send return until completion callback")         \
    X(TransportTaskQueueWaitNotified, "kv_transport_task_queue_wait_notified_total", COUNTER,     \
      "Total KV transport task queue waits completed by notification")                            \
    X(TransportTaskQueueWaitTimeouts, "kv_transport_task_queue_wait_timeout_total", COUNTER,      \
      "Total KV transport task queue waits completed by timeout")                                 \
    X(TransportTaskQueueNotifies, "kv_transport_task_queue_notify_total", COUNTER,                \
      "Total KV transport task queue condition-variable notifications")                           \
    X(FakeBackendTaskQueueDuration, "kv_fake_backend_task_queue_duration_seconds", HISTOGRAM,     \
      "KV fake backend task queue wait duration from provider Send until fake worker starts")     \
    X(FakeBackendTaskProcessDuration, "kv_fake_backend_task_process_duration_seconds", HISTOGRAM, \
      "KV fake backend processing duration from fake worker start until completion is published") \
    X(ExporterUp, "kv_metrics_exporter_up", GAUGE,                                                \
      "Whether the KV standalone metrics exporter is running")                                    \
    X(ExporterHttpRequests, "kv_metrics_exporter_http_requests_total", COUNTER,                   \
      "Total HTTP requests served by the KV metrics endpoint")

enum class MetricId : std::uint8_t {
#define KV_DECLARE_METRIC_ID(id, name, type, documentation) id,
    KV_BUILTIN_METRIC_LIST(KV_DECLARE_METRIC_ID)
#undef KV_DECLARE_METRIC_ID
        COUNT,
};

inline constexpr std::size_t kBuiltinMetricCount = static_cast<std::size_t>(MetricId::COUNT);

constexpr std::size_t ToIndex(MetricId id) noexcept { return static_cast<std::size_t>(id); }

constexpr std::string_view MetricName(MetricId id) noexcept
{
    switch (id) {
#define KV_METRIC_NAME_CASE(metricId, name, type, documentation) \
    case MetricId::metricId: return name;
        KV_BUILTIN_METRIC_LIST(KV_METRIC_NAME_CASE)
#undef KV_METRIC_NAME_CASE
        case MetricId::COUNT: return {};
    }
    return {};
}

// Compatibility names for dynamic string-based callers.
namespace Names {
#define KV_DECLARE_METRIC_NAME(id, name, type, documentation) \
    inline constexpr std::string_view id = name;
KV_BUILTIN_METRIC_LIST(KV_DECLARE_METRIC_NAME)
#undef KV_DECLARE_METRIC_NAME
}  // namespace Names

}  // namespace kv::metrics
