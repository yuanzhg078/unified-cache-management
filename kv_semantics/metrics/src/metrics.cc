#include "kv_metrics/metrics.h"
#include <atomic>
#include <mutex>
#include <utility>

namespace kv::metrics {
namespace {

std::mutex gBackendMutex;
std::shared_ptr<MetricsBackend> gBackend;
// The owner is retained in gBackend. Business threads use the raw pointer to avoid
// shared_ptr reference-count traffic on every metric update. The lifecycle contract
// requires all business threads to stop before Shutdown().
std::atomic<MetricsBackend*> gBackendFast{nullptr};

MetricDescriptor MakeBuiltinDescriptor(std::string_view name, MetricType type,
                                       std::string documentation)
{
    switch (type) {
        case MetricType::COUNTER:
        case MetricType::GAUGE: return {std::string{name}, type, std::move(documentation), {}};
        case MetricType::HISTOGRAM:
            return {
                std::string{name},
                type,
                std::move(documentation),
                {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0}
            };
    }
    return {};
}

std::shared_ptr<MetricsBackend> LoadBackend()
{
    return std::atomic_load_explicit(&gBackend, std::memory_order_acquire);
}

MetricsBackend* LoadBackendFast() noexcept { return gBackendFast.load(std::memory_order_acquire); }

}  // namespace

bool Initialize(std::shared_ptr<MetricsBackend> backend, std::string* error)
{
    if (!backend) {
        if (error != nullptr) { *error = "metrics backend is null"; }
        return false;
    }

    std::lock_guard<std::mutex> lock{gBackendMutex};
    if (LoadBackend()) {
        if (error != nullptr) { *error = "metrics backend is already initialized"; }
        return false;
    }
    if (!backend->Start()) {
        if (error != nullptr) { *error = backend->LastError(); }
        return false;
    }
    auto* const backendFast = backend.get();
    std::atomic_store_explicit(&gBackend, std::move(backend), std::memory_order_release);
    gBackendFast.store(backendFast, std::memory_order_release);
    return true;
}

void Shutdown()
{
    gBackendFast.store(nullptr, std::memory_order_release);
    std::shared_ptr<MetricsBackend> backend;
    {
        std::lock_guard<std::mutex> lock{gBackendMutex};
        backend = std::atomic_exchange_explicit(&gBackend, std::shared_ptr<MetricsBackend>{},
                                                std::memory_order_acq_rel);
    }
    if (backend) {
        backend->Flush();
        backend->Stop();
    }
}

void Flush()
{
    auto backend = LoadBackend();
    if (backend) { backend->Flush(); }
}

bool IsEnabled() noexcept { return LoadBackendFast() != nullptr; }

MetricTimer StartTimer() noexcept
{
    if (!IsEnabled()) { return {}; }
    return {std::chrono::steady_clock::now(), true};
}

void Update(std::string_view name, double value) noexcept
{
    auto* backend = LoadBackendFast();
    if (backend) { backend->Update(name, value); }
}

void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept
{
    if (updates == nullptr || count == 0) { return; }
    auto* backend = LoadBackendFast();
    if (backend) { backend->UpdateBuiltinBatch(updates, count); }
}

std::vector<MetricDescriptor> DefaultKvMetricDescriptors()
{
    std::vector<MetricDescriptor> descriptors;
    descriptors.reserve(kBuiltinMetricCount);
#define KV_APPEND_BUILTIN_DESCRIPTOR(id, name, type, documentation) \
    descriptors.emplace_back(MakeBuiltinDescriptor(name, MetricType::type, documentation));
    KV_BUILTIN_METRIC_LIST(KV_APPEND_BUILTIN_DESCRIPTOR)
#undef KV_APPEND_BUILTIN_DESCRIPTOR
    return descriptors;
}

}  // namespace kv::metrics
