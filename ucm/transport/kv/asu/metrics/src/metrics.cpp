#include "asu_metrics/metrics.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace UC::ASU::Metrics {
namespace {

std::mutex gBackendMutex;
std::shared_ptr<MetricsBackend> gBackend;
// The owner is retained in gBackend. Business threads use the raw pointer to avoid
// shared_ptr reference-count traffic on every metric update. The lifecycle contract
// requires all business threads to stop before Shutdown().
std::atomic<MetricsBackend*> gBackendFast{nullptr};

class NoopMetricsBackend final : public MetricsBackend {
public:
    bool Start() override { return true; }
    void Add(std::string_view, double) noexcept override {}
    void Set(std::string_view, double) noexcept override {}
    void Observe(std::string_view, double) noexcept override {}
    void UpdateBuiltinBatch(const BuiltinMetricUpdate*, std::size_t) noexcept override {}
    void Flush() override {}
    void Stop() override {}
    std::string LastError() const override { return {}; }
};

std::shared_ptr<MetricsBackend> LoadBackend()
{
    return std::atomic_load_explicit(&gBackend, std::memory_order_acquire);
}

MetricsBackend* LoadBackendFast() noexcept
{
    return gBackendFast.load(std::memory_order_acquire);
}

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

void Add(std::string_view name, double delta) noexcept
{
    auto* backend = LoadBackendFast();
    if (backend) { backend->Add(name, delta); }
}

void Set(std::string_view name, double value) noexcept
{
    auto* backend = LoadBackendFast();
    if (backend) { backend->Set(name, value); }
}

void Observe(std::string_view name, double value) noexcept
{
    auto* backend = LoadBackendFast();
    if (backend) { backend->Observe(name, value); }
}

void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept
{
    if (updates == nullptr || count == 0) { return; }
    auto* backend = LoadBackendFast();
    if (backend) { backend->UpdateBuiltinBatch(updates, count); }
}

std::shared_ptr<MetricsBackend> CreateNoopMetricsBackend()
{
    return std::make_shared<NoopMetricsBackend>();
}

}  // namespace UC::ASU::Metrics
