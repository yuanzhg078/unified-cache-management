#include "asu_metrics/metrics.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace UC::ASU::Metrics {
namespace {

std::mutex gBackendMutex;
std::shared_ptr<MetricsBackend> gBackend;

class NoopMetricsBackend final : public MetricsBackend {
public:
    bool Start() override { return true; }
    void Add(std::string_view, double) noexcept override {}
    void Set(std::string_view, double) noexcept override {}
    void Observe(std::string_view, double) noexcept override {}
    void Flush() override {}
    void Stop() override {}
    std::string LastError() const override { return {}; }
};

std::shared_ptr<MetricsBackend> LoadBackend()
{
    return std::atomic_load_explicit(&gBackend, std::memory_order_acquire);
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
    std::atomic_store_explicit(&gBackend, std::move(backend), std::memory_order_release);
    return true;
}

void Shutdown()
{
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

bool IsEnabled() { return static_cast<bool>(LoadBackend()); }

void Add(std::string_view name, double delta) noexcept
{
    auto backend = LoadBackend();
    if (backend) { backend->Add(name, delta); }
}

void Set(std::string_view name, double value) noexcept
{
    auto backend = LoadBackend();
    if (backend) { backend->Set(name, value); }
}

void Observe(std::string_view name, double value) noexcept
{
    auto backend = LoadBackend();
    if (backend) { backend->Observe(name, value); }
}

std::shared_ptr<MetricsBackend> CreateNoopMetricsBackend()
{
    return std::make_shared<NoopMetricsBackend>();
}

}  // namespace UC::ASU::Metrics
