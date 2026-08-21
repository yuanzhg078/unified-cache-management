#pragma once

#include <string_view>

namespace UC::ASU::Metrics::Names {

inline constexpr std::string_view QueryRequests = "asu_client_query_requests_total";
inline constexpr std::string_view QueryEntries = "asu_client_query_entries_total";
inline constexpr std::string_view QueryErrors = "asu_client_query_errors_total";
inline constexpr std::string_view QuerySubmitDuration =
    "asu_client_query_submit_duration_seconds";

inline constexpr std::string_view LoadRequests = "asu_client_load_requests_total";
inline constexpr std::string_view LoadEntries = "asu_client_load_entries_total";
inline constexpr std::string_view LoadErrors = "asu_client_load_errors_total";
inline constexpr std::string_view LoadSubmitDuration =
    "asu_client_load_submit_duration_seconds";

inline constexpr std::string_view StoreRequests = "asu_client_store_requests_total";
inline constexpr std::string_view StoreEntries = "asu_client_store_entries_total";
inline constexpr std::string_view StoreErrors = "asu_client_store_errors_total";
inline constexpr std::string_view StoreSubmitDuration =
    "asu_client_store_submit_duration_seconds";

inline constexpr std::string_view BatchLoadRequests = "asu_client_batch_load_requests_total";
inline constexpr std::string_view BatchLoadEntries = "asu_client_batch_load_entries_total";
inline constexpr std::string_view BatchLoadErrors = "asu_client_batch_load_errors_total";
inline constexpr std::string_view BatchLoadSubmitDuration =
    "asu_client_batch_load_submit_duration_seconds";

inline constexpr std::string_view BatchStoreRequests = "asu_client_batch_store_requests_total";
inline constexpr std::string_view BatchStoreEntries = "asu_client_batch_store_entries_total";
inline constexpr std::string_view BatchStoreErrors = "asu_client_batch_store_errors_total";
inline constexpr std::string_view BatchStoreSubmitDuration =
    "asu_client_batch_store_submit_duration_seconds";

inline constexpr std::string_view DeleteRequests = "asu_client_delete_requests_total";
inline constexpr std::string_view DeleteEntries = "asu_client_delete_entries_total";
inline constexpr std::string_view DeleteErrors = "asu_client_delete_errors_total";
inline constexpr std::string_view DeleteSubmitDuration =
    "asu_client_delete_submit_duration_seconds";

inline constexpr std::string_view WaitRequests = "asu_client_wait_requests_total";
inline constexpr std::string_view WaitErrors = "asu_client_wait_errors_total";
inline constexpr std::string_view WaitDuration = "asu_client_wait_duration_seconds";

inline constexpr std::string_view ExporterUp = "asu_metrics_exporter_up";
inline constexpr std::string_view ExporterHttpRequests =
    "asu_metrics_exporter_http_requests_total";

}  // namespace UC::ASU::Metrics::Names
