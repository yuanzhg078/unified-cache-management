#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace UC::ASU::Metrics {

// One definition table generates the built-in IDs, names, and descriptors.
#define ASU_BUILTIN_METRIC_LIST(X)                                                                 \
    X(QueryRequests, "asu_client_query_requests_total", COUNTER,                                   \
      "Total ASU client query submissions")                                                        \
    X(QueryEntries, "asu_client_query_entries_total", COUNTER,                                     \
      "Total keys submitted to ASU client query")                                                  \
    X(QueryErrors, "asu_client_query_errors_total", COUNTER,                                       \
      "Total failed ASU client query submissions")                                                 \
    X(QuerySubmitDuration, "asu_client_query_submit_duration_seconds", HISTOGRAM,                  \
      "ASU client query submission duration in seconds")                                           \
    X(LoadRequests, "asu_client_load_requests_total", COUNTER,                                     \
      "Total ASU client load submissions")                                                         \
    X(LoadEntries, "asu_client_load_entries_total", COUNTER,                                       \
      "Total entries submitted to ASU client load")                                                \
    X(LoadErrors, "asu_client_load_errors_total", COUNTER,                                         \
      "Total failed ASU client load submissions")                                                  \
    X(LoadSubmitDuration, "asu_client_load_submit_duration_seconds", HISTOGRAM,                    \
      "ASU client load submission duration in seconds")                                            \
    X(StoreRequests, "asu_client_store_requests_total", COUNTER,                                   \
      "Total ASU client store submissions")                                                        \
    X(StoreEntries, "asu_client_store_entries_total", COUNTER,                                     \
      "Total entries submitted to ASU client store")                                               \
    X(StoreErrors, "asu_client_store_errors_total", COUNTER,                                       \
      "Total failed ASU client store submissions")                                                 \
    X(StoreSubmitDuration, "asu_client_store_submit_duration_seconds", HISTOGRAM,                  \
      "ASU client store submission duration in seconds")                                           \
    X(BatchLoadRequests, "asu_client_batch_load_requests_total", COUNTER,                          \
      "Total ASU client batch-load submissions")                                                   \
    X(BatchLoadEntries, "asu_client_batch_load_entries_total", COUNTER,                            \
      "Total entries submitted to ASU client batch-load")                                          \
    X(BatchLoadErrors, "asu_client_batch_load_errors_total", COUNTER,                              \
      "Total failed ASU client batch-load submissions")                                            \
    X(BatchLoadSubmitDuration, "asu_client_batch_load_submit_duration_seconds", HISTOGRAM,         \
      "ASU client batch-load submission duration in seconds")                                      \
    X(BatchStoreRequests, "asu_client_batch_store_requests_total", COUNTER,                        \
      "Total ASU client batch-store submissions")                                                  \
    X(BatchStoreEntries, "asu_client_batch_store_entries_total", COUNTER,                          \
      "Total entries submitted to ASU client batch-store")                                         \
    X(BatchStoreErrors, "asu_client_batch_store_errors_total", COUNTER,                            \
      "Total failed ASU client batch-store submissions")                                           \
    X(BatchStoreSubmitDuration, "asu_client_batch_store_submit_duration_seconds", HISTOGRAM,       \
      "ASU client batch-store submission duration in seconds")                                     \
    X(DeleteRequests, "asu_client_delete_requests_total", COUNTER,                                 \
      "Total ASU client delete submissions")                                                       \
    X(DeleteEntries, "asu_client_delete_entries_total", COUNTER,                                   \
      "Total keys submitted to ASU client delete")                                                 \
    X(DeleteErrors, "asu_client_delete_errors_total", COUNTER,                                     \
      "Total failed ASU client delete submissions")                                                \
    X(DeleteSubmitDuration, "asu_client_delete_submit_duration_seconds", HISTOGRAM,                \
      "ASU client delete submission duration in seconds")                                          \
    X(WaitRequests, "asu_client_wait_requests_total", COUNTER, "Total ASU client wait calls")      \
    X(WaitErrors, "asu_client_wait_errors_total", COUNTER, "Total failed ASU client wait calls")   \
    X(WaitDuration, "asu_client_wait_duration_seconds", HISTOGRAM,                                 \
      "ASU client wait duration in seconds")                                                       \
    X(ClientTaskEnqueueDuration, "asu_client_task_enqueue_duration_seconds", HISTOGRAM,            \
      "ASU client task duration from API entry until client queue enqueue")                        \
    X(ClientTaskQueueDuration, "asu_client_task_queue_duration_seconds", HISTOGRAM,                \
      "ASU client task queue wait duration")                                                       \
    X(ClientTaskProcessDuration, "asu_client_task_process_duration_seconds", HISTOGRAM,            \
      "ASU client worker processing duration until all transport tasks are submitted")             \
    X(ClientTaskPreSendDuration, "asu_client_task_pre_send_duration_seconds", HISTOGRAM,           \
      "ASU client task duration from enqueue until all transport tasks reach provider Send")       \
    X(ClientTaskSendDuration, "asu_client_task_send_duration_seconds", HISTOGRAM,                  \
      "ASU client task duration from enqueue until all transport Send calls return")               \
    X(ClientTaskDuration, "asu_client_task_duration_seconds", HISTOGRAM,                           \
      "ASU client task end-to-end duration from API entry to completion")                          \
    X(ClientTaskQueueWaits, "asu_client_task_queue_wait_total", COUNTER,                           \
      "Total ASU client task queue condition-variable waits")                                      \
    X(ClientTaskQueueNotifies, "asu_client_task_queue_notify_total", COUNTER,                      \
      "Total ASU client task queue condition-variable notifications")                              \
    X(TransportTaskPreSendDuration, "asu_transport_task_pre_send_duration_seconds", HISTOGRAM,     \
      "ASU transport task duration from client dispatch until immediately before Send")            \
    X(TransportTaskQueueDuration, "asu_transport_task_queue_duration_seconds", HISTOGRAM,          \
      "ASU transport task queue wait duration")                                                    \
    X(TransportTaskProcessDuration, "asu_transport_task_process_duration_seconds", HISTOGRAM,      \
      "ASU transport executor processing duration until immediately before Send")                  \
    X(TransportTaskSendDuration, "asu_transport_task_send_duration_seconds", HISTOGRAM,            \
      "ASU transport task duration from client dispatch until Send returns")                       \
    X(TransportTaskCompletionDuration, "asu_transport_task_completion_duration_seconds",           \
      HISTOGRAM, "ASU transport task duration from Send return until completion callback")         \
    X(TransportTaskQueueWaits, "asu_transport_task_queue_wait_total", COUNTER,                     \
      "Total ASU transport task queue condition-variable waits")                                   \
    X(TransportTaskQueueNotifies, "asu_transport_task_queue_notify_total", COUNTER,                \
      "Total ASU transport task queue condition-variable notifications")                           \
    X(FakeBackendTaskQueueDuration, "asu_fake_backend_task_queue_duration_seconds", HISTOGRAM,     \
      "ASU fake backend task queue wait duration from provider Send until fake worker starts")     \
    X(FakeBackendTaskProcessDuration, "asu_fake_backend_task_process_duration_seconds", HISTOGRAM, \
      "ASU fake backend processing duration from fake worker start until completion is published") \
    X(ExporterUp, "asu_metrics_exporter_up", GAUGE,                                                \
      "Whether the ASU standalone metrics exporter is running")                                    \
    X(ExporterHttpRequests, "asu_metrics_exporter_http_requests_total", COUNTER,                   \
      "Total HTTP requests served by the ASU metrics endpoint")

enum class MetricId : std::uint8_t {
#define ASU_DECLARE_METRIC_ID(id, name, type, documentation) id,
    ASU_BUILTIN_METRIC_LIST(ASU_DECLARE_METRIC_ID)
#undef ASU_DECLARE_METRIC_ID
        COUNT,
};

inline constexpr std::size_t kBuiltinMetricCount = static_cast<std::size_t>(MetricId::COUNT);

constexpr std::size_t ToIndex(MetricId id) noexcept { return static_cast<std::size_t>(id); }

constexpr std::string_view MetricName(MetricId id) noexcept
{
    switch (id) {
#define ASU_METRIC_NAME_CASE(metricId, name, type, documentation) \
    case MetricId::metricId: return name;
        ASU_BUILTIN_METRIC_LIST(ASU_METRIC_NAME_CASE)
#undef ASU_METRIC_NAME_CASE
        case MetricId::COUNT: return {};
    }
    return {};
}

// Compatibility names for dynamic string-based callers.
namespace Names {
#define ASU_DECLARE_METRIC_NAME(id, name, type, documentation) \
    inline constexpr std::string_view id = name;
ASU_BUILTIN_METRIC_LIST(ASU_DECLARE_METRIC_NAME)
#undef ASU_DECLARE_METRIC_NAME
}  // namespace Names

}  // namespace UC::ASU::Metrics
