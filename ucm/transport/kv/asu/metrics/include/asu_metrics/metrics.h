#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace UC::ASU::Metrics {

enum class MetricType { COUNTER = 0, GAUGE, HISTOGRAM };

struct MetricDescriptor {
    std::string name;
    MetricType type{MetricType::COUNTER};
    std::string documentation;
    std::vector<double> buckets;
};

struct StandaloneMetricsConfig {
    std::string definitionPath;
    std::string metricPrefix{"ucm:"};
    std::string listenAddress{"127.0.0.1"};
    std::uint16_t port{9108};
    std::string metricsPath{"/metrics"};
    std::string healthPath{"/healthz"};
    std::map<std::string, std::string> constantLabels;
    std::vector<MetricDescriptor> descriptors;
};

class MetricsBackend {
public:
    virtual ~MetricsBackend() = default;

    virtual bool Start() = 0;
    virtual void Add(std::string_view name, double delta) noexcept = 0;
    virtual void Set(std::string_view name, double value) noexcept = 0;
    virtual void Observe(std::string_view name, double value) noexcept = 0;
    virtual void Flush() = 0;
    virtual void Stop() = 0;
    virtual std::string LastError() const = 0;
};

bool Initialize(std::shared_ptr<MetricsBackend> backend, std::string* error = nullptr);
void Shutdown();
void Flush();
bool IsEnabled();

void Add(std::string_view name, double delta = 1.0) noexcept;
void Set(std::string_view name, double value) noexcept;
void Observe(std::string_view name, double value) noexcept;

std::shared_ptr<MetricsBackend> CreateNoopMetricsBackend();
std::shared_ptr<MetricsBackend> CreateStandaloneMetricsBackend(StandaloneMetricsConfig config);
std::vector<MetricDescriptor> DefaultAsuMetricDescriptors();

}  // namespace UC::ASU::Metrics
