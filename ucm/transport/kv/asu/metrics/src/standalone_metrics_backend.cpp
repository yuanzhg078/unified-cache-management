#include "asu_metrics/metrics.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "asu_metrics/metric_names.h"

namespace UC::ASU::Metrics {
namespace {

constexpr int kListenBacklog = 16;
constexpr std::size_t kMaxRequestBytes = 4096;

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return {}; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string StripComment(const std::string& line)
{
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '\'' && !inDoubleQuote) { inSingleQuote = !inSingleQuote; }
        if (line[index] == '"' && !inSingleQuote) { inDoubleQuote = !inDoubleQuote; }
        if (line[index] == '#' && !inSingleQuote && !inDoubleQuote) {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string Unquote(std::string value)
{
    value = Trim(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool ParseBuckets(const std::string& value, std::vector<double>& buckets)
{
    auto text = Trim(value);
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') { return false; }
    text = text.substr(1, text.size() - 2);
    std::stringstream stream{text};
    std::string item;
    std::vector<double> parsed;
    try {
        while (std::getline(stream, item, ',')) {
            item = Trim(item);
            if (item.empty()) { continue; }
            std::size_t consumed = 0;
            const auto number = std::stod(item, &consumed);
            if (consumed != item.size() || !std::isfinite(number)) { return false; }
            parsed.emplace_back(number);
        }
    } catch (...) {
        return false;
    }
    if (!std::is_sorted(parsed.begin(), parsed.end()) ||
        std::adjacent_find(parsed.begin(), parsed.end()) != parsed.end()) {
        return false;
    }
    buckets = std::move(parsed);
    return true;
}

bool IsValidMetricName(const std::string& name)
{
    if (name.empty()) { return false; }
    const auto first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || name.front() == '_' || name.front() == ':')) { return false; }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == ':';
    });
}

std::string EscapeHelp(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string EscapeLabel(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '"') {
            escaped += "\\\"";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

const char* PrometheusTypeName(MetricType type)
{
    switch (type) {
        case MetricType::COUNTER: return "counter";
        case MetricType::GAUGE: return "gauge";
        case MetricType::HISTOGRAM: return "histogram";
        default: return "untyped";
    }
}

MetricDescriptor Counter(std::string_view name, std::string documentation)
{
    return {std::string{name}, MetricType::COUNTER, std::move(documentation), {}};
}

MetricDescriptor Gauge(std::string_view name, std::string documentation)
{
    return {std::string{name}, MetricType::GAUGE, std::move(documentation), {}};
}

MetricDescriptor Histogram(std::string_view name, std::string documentation)
{
    return {std::string{name}, MetricType::HISTOGRAM, std::move(documentation),
            {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0}};
}

bool LoadYamlDefinitions(const std::string& path, std::string& metricPrefix,
                         std::vector<MetricDescriptor>& descriptors, std::string& error)
{
    std::ifstream input{path};
    if (!input.is_open()) {
        error = "failed to open metrics definition file: " + path;
        return false;
    }

    bool inMetricSection = false;
    MetricType sectionType = MetricType::COUNTER;
    MetricDescriptor* current = nullptr;
    std::string rawLine;
    std::size_t lineNumber = 0;
    while (std::getline(input, rawLine)) {
        ++lineNumber;
        const auto content = Trim(StripComment(rawLine));
        if (content.empty()) { continue; }

        if (content.rfind("metric_prefix:", 0) == 0) {
            metricPrefix = Unquote(content.substr(std::strlen("metric_prefix:")));
            continue;
        }
        if (content == "counter:" || content == "gauge:" || content == "histogram:") {
            inMetricSection = true;
            current = nullptr;
            sectionType = content == "counter:"   ? MetricType::COUNTER
                          : content == "gauge:"   ? MetricType::GAUGE
                                                   : MetricType::HISTOGRAM;
            continue;
        }
        if (!rawLine.empty() && rawLine.front() != ' ' && rawLine.front() != '\t') {
            inMetricSection = false;
            current = nullptr;
            continue;
        }
        if (!inMetricSection) { continue; }

        if (content.rfind("- name:", 0) == 0) {
            MetricDescriptor descriptor;
            descriptor.name = Unquote(content.substr(std::strlen("- name:")));
            descriptor.type = sectionType;
            descriptors.emplace_back(std::move(descriptor));
            current = &descriptors.back();
            continue;
        }
        if (current == nullptr) { continue; }
        if (content.rfind("documentation:", 0) == 0) {
            current->documentation =
                Unquote(content.substr(std::strlen("documentation:")));
        } else if (content.rfind("buckets:", 0) == 0) {
            if (!ParseBuckets(content.substr(std::strlen("buckets:")), current->buckets)) {
                error = "invalid histogram buckets at " + path + ":" +
                        std::to_string(lineNumber);
                return false;
            }
        }
    }
    return true;
}

class ThreadBufferedMetricsCollector {
public:
    ~ThreadBufferedMetricsCollector() { StopAggregation(); }

    bool Register(const MetricDescriptor& descriptor, std::string& error)
    {
        if (!IsValidMetricName(descriptor.name)) {
            error = "invalid metric name: " + descriptor.name;
            return false;
        }
        if (descriptor.type == MetricType::HISTOGRAM && descriptor.buckets.empty()) {
            error = "histogram has no buckets: " + descriptor.name;
            return false;
        }
        if (!std::is_sorted(descriptor.buckets.begin(), descriptor.buckets.end()) ||
            std::adjacent_find(descriptor.buckets.begin(), descriptor.buckets.end()) !=
                descriptor.buckets.end()) {
            error = "histogram buckets must be sorted and unique: " + descriptor.name;
            return false;
        }
        if (metricIds_.find(descriptor.name) != metricIds_.end()) {
            error = "duplicate metric name: " + descriptor.name;
            return false;
        }
        metricIds_.emplace(descriptor.name, descriptors_.size());
        descriptors_.emplace_back(descriptor);
        snapshot_.emplace_back(MakeMetricState(descriptor));
        return true;
    }

    void StartAggregation(std::uint32_t intervalMs)
    {
        intervalMs_ = std::chrono::milliseconds{std::max<std::uint32_t>(1, intervalMs)};
        if (aggregatorRunning_.exchange(true, std::memory_order_acq_rel)) { return; }
        aggregator_ = std::thread(&ThreadBufferedMetricsCollector::AggregatorLoop, this);
    }

    void StopAggregation()
    {
        if (!aggregatorRunning_.exchange(false, std::memory_order_acq_rel)) { return; }
        wakeup_.notify_all();
        if (aggregator_.joinable()) { aggregator_.join(); }
        Aggregate();
    }

    void Add(std::string_view name, double delta) noexcept
    {
        if (!std::isfinite(delta) || delta < 0.0) { return; }
        std::size_t id = 0;
        if (!FindMetric(name, id) || descriptors_[id].type != MetricType::COUNTER) { return; }
        auto buffer = GetThreadBuffer();
        const auto slot = buffer->writeIndex.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock{buffer->slots[slot].mutex};
        buffer->slots[slot].metrics[id].value += delta;
    }

    void Set(std::string_view name, double value) noexcept
    {
        if (!std::isfinite(value)) { return; }
        std::size_t id = 0;
        if (!FindMetric(name, id) || descriptors_[id].type != MetricType::GAUGE) { return; }
        auto buffer = GetThreadBuffer();
        const auto slot = buffer->writeIndex.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock{buffer->slots[slot].mutex};
        auto& metric = buffer->slots[slot].metrics[id];
        metric.value = value;
        metric.sequence = gaugeSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        metric.hasGaugeValue = true;
    }

    void Observe(std::string_view name, double value) noexcept
    {
        if (!std::isfinite(value)) { return; }
        std::size_t id = 0;
        if (!FindMetric(name, id) || descriptors_[id].type != MetricType::HISTOGRAM) { return; }
        auto buffer = GetThreadBuffer();
        const auto slot = buffer->writeIndex.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock{buffer->slots[slot].mutex};
        auto& metric = buffer->slots[slot].metrics[id];
        metric.sum += value;
        ++metric.count;
        const auto& buckets = descriptors_[id].buckets;
        const auto bucket = static_cast<std::size_t>(
            std::lower_bound(buckets.begin(), buckets.end(), value) - buckets.begin());
        ++metric.bucketCounts[bucket];
    }

    void SetGaugeDirect(std::string_view name, double value) noexcept
    {
        std::size_t id = 0;
        if (!std::isfinite(value) || !FindMetric(name, id) ||
            descriptors_[id].type != MetricType::GAUGE) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock{snapshotMutex_};
        auto& metric = snapshot_[id];
        metric.value = value;
        metric.sequence = gaugeSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        metric.hasGaugeValue = true;
    }

    void Flush() { Aggregate(); }

    std::string Render(const std::string& prefix,
                       const std::map<std::string, std::string>& labels) const
    {
        std::shared_lock<std::shared_mutex> lock{snapshotMutex_};
        std::ostringstream output;
        output << std::setprecision(17);
        for (std::size_t id = 0; id < descriptors_.size(); ++id) {
            const auto& descriptor = descriptors_[id];
            const auto& state = snapshot_[id];
            const auto fullName = prefix + descriptor.name;
            output << "# HELP " << fullName << ' ' << EscapeHelp(descriptor.documentation) << '\n';
            output << "# TYPE " << fullName << ' ' << PrometheusTypeName(descriptor.type)
                   << '\n';
            if (descriptor.type == MetricType::HISTOGRAM) {
                std::uint64_t cumulative = 0;
                for (std::size_t index = 0; index < descriptor.buckets.size(); ++index) {
                    cumulative += state.bucketCounts[index];
                    output << fullName << "_bucket"
                           << RenderLabels(labels, "le", FormatNumber(descriptor.buckets[index]))
                           << ' ' << cumulative << '\n';
                }
                output << fullName << "_bucket" << RenderLabels(labels, "le", "+Inf") << ' '
                       << state.count << '\n';
                output << fullName << "_sum" << RenderLabels(labels) << ' ' << state.sum << '\n';
                output << fullName << "_count" << RenderLabels(labels) << ' ' << state.count
                       << '\n';
            } else {
                output << fullName << RenderLabels(labels) << ' ' << state.value << '\n';
            }
        }
        return output.str();
    }

private:
    struct MetricState {
        double value{0.0};
        std::uint64_t sequence{0};
        bool hasGaugeValue{false};
        double sum{0.0};
        std::uint64_t count{0};
        // Histogram interval counts: bucket i is (-inf, le_i], final bucket is (+last, +inf).
        std::vector<std::uint64_t> bucketCounts;
    };

    struct DeltaSlot {
        std::mutex mutex;
        std::vector<MetricState> metrics;
    };

    struct ThreadBuffer {
        explicit ThreadBuffer(const std::vector<MetricDescriptor>& descriptors)
        {
            for (auto& slot : slots) {
                slot.metrics.reserve(descriptors.size());
                for (const auto& descriptor : descriptors) {
                    slot.metrics.emplace_back(MakeMetricState(descriptor));
                }
            }
        }

        std::atomic<int> writeIndex{0};
        DeltaSlot slots[2];
    };

    struct ThreadLocalBufferCache {
        const ThreadBufferedMetricsCollector* owner{nullptr};
        std::weak_ptr<ThreadBuffer> buffer;
    };

    static MetricState MakeMetricState(const MetricDescriptor& descriptor)
    {
        MetricState state;
        if (descriptor.type == MetricType::HISTOGRAM) {
            state.bucketCounts.assign(descriptor.buckets.size() + 1, 0);
        }
        return state;
    }

    bool FindMetric(std::string_view name, std::size_t& id) const noexcept
    {
        const auto iter = metricIds_.find(std::string{name});
        if (iter == metricIds_.end()) { return false; }
        id = iter->second;
        return true;
    }

    std::shared_ptr<ThreadBuffer> GetThreadBuffer()
    {
        static thread_local ThreadLocalBufferCache cache;
        if (cache.owner == this) {
            if (auto buffer = cache.buffer.lock()) { return buffer; }
        }
        auto buffer = std::make_shared<ThreadBuffer>(descriptors_);
        {
            std::lock_guard<std::mutex> lock{buffersMutex_};
            buffers_.emplace_back(buffer);
        }
        cache.owner = this;
        cache.buffer = buffer;
        return buffer;
    }

    void AggregatorLoop()
    {
        std::unique_lock<std::mutex> lock{wakeupMutex_};
        while (aggregatorRunning_.load(std::memory_order_acquire)) {
            wakeup_.wait_for(lock, intervalMs_);
            if (!aggregatorRunning_.load(std::memory_order_acquire)) { break; }
            lock.unlock();
            Aggregate();
            lock.lock();
        }
    }

    void Aggregate()
    {
        std::lock_guard<std::mutex> aggregateLock{aggregateMutex_};
        std::vector<std::shared_ptr<ThreadBuffer>> buffers;
        {
            std::lock_guard<std::mutex> lock{buffersMutex_};
            buffers = buffers_;
        }
        std::unique_lock<std::shared_mutex> snapshotLock{snapshotMutex_};
        for (const auto& buffer : buffers) {
            const int oldSlot = buffer->writeIndex.fetch_xor(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> slotLock{buffer->slots[oldSlot].mutex};
            auto& source = buffer->slots[oldSlot].metrics;
            for (std::size_t id = 0; id < source.size(); ++id) {
                auto& from = source[id];
                auto& to = snapshot_[id];
                switch (descriptors_[id].type) {
                    case MetricType::COUNTER:
                        to.value += from.value;
                        from.value = 0.0;
                        break;
                    case MetricType::GAUGE:
                        if (from.hasGaugeValue && from.sequence >= to.sequence) {
                            to.value = from.value;
                            to.sequence = from.sequence;
                            to.hasGaugeValue = true;
                        }
                        from.hasGaugeValue = false;
                        from.sequence = 0;
                        break;
                    case MetricType::HISTOGRAM:
                        to.sum += from.sum;
                        to.count += from.count;
                        for (std::size_t bucket = 0; bucket < from.bucketCounts.size(); ++bucket) {
                            to.bucketCounts[bucket] += from.bucketCounts[bucket];
                            from.bucketCounts[bucket] = 0;
                        }
                        from.sum = 0.0;
                        from.count = 0;
                        break;
                }
            }
        }
    }

    static std::string FormatNumber(double value)
    {
        std::ostringstream output;
        output << std::setprecision(17) << value;
        return output.str();
    }

    static std::string RenderLabels(const std::map<std::string, std::string>& labels,
                                    const std::string& extraName = {},
                                    const std::string& extraValue = {})
    {
        if (labels.empty() && extraName.empty()) { return {}; }
        std::ostringstream output;
        output << '{';
        bool first = true;
        for (const auto& label : labels) {
            if (!first) { output << ','; }
            output << label.first << "=\"" << EscapeLabel(label.second) << '"';
            first = false;
        }
        if (!extraName.empty()) {
            if (!first) { output << ','; }
            output << extraName << "=\"" << EscapeLabel(extraValue) << '"';
        }
        output << '}';
        return output.str();
    }

    std::vector<MetricDescriptor> descriptors_;
    std::unordered_map<std::string, std::size_t> metricIds_;
    mutable std::shared_mutex snapshotMutex_;
    std::vector<MetricState> snapshot_;
    std::atomic<std::uint64_t> gaugeSequence_{0};
    std::mutex buffersMutex_;
    std::vector<std::shared_ptr<ThreadBuffer>> buffers_;
    std::mutex aggregateMutex_;
    std::chrono::milliseconds intervalMs_{500};
    std::atomic<bool> aggregatorRunning_{false};
    std::mutex wakeupMutex_;
    std::condition_variable wakeup_;
    std::thread aggregator_;
};

class MetricsHttpServer {
public:
    MetricsHttpServer(ThreadBufferedMetricsCollector& registry, StandaloneMetricsConfig config)
        : registry_(registry), config_(std::move(config))
    {}

    ~MetricsHttpServer() { Stop(); }

    bool Start(std::string& error)
    {
        if (running_.load(std::memory_order_acquire)) { return true; }

        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        struct addrinfo* addresses = nullptr;
        const auto portText = std::to_string(config_.port);
        const auto rc = getaddrinfo(config_.listenAddress.c_str(), portText.c_str(), &hints,
                                    &addresses);
        if (rc != 0) {
            error = "failed to resolve metrics listen address: " +
                    std::string{gai_strerror(rc)};
            return false;
        }

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            listenFd_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (listenFd_ < 0) { continue; }
            int reuse = 1;
            (void)setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (bind(listenFd_, address->ai_addr, address->ai_addrlen) == 0 &&
                listen(listenFd_, kListenBacklog) == 0) {
                break;
            }
            close(listenFd_);
            listenFd_ = -1;
        }
        freeaddrinfo(addresses);

        if (listenFd_ < 0) {
            error = "failed to listen on " + config_.listenAddress + ':' +
                    std::to_string(config_.port) + ": " + std::strerror(errno);
            return false;
        }

        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&MetricsHttpServer::Serve, this);
        return true;
    }

    void Stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) { return; }
        const int fd = listenFd_;
        if (fd >= 0) {
            (void)shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if (worker_.joinable()) { worker_.join(); }
        listenFd_ = -1;
    }

private:
    void Serve()
    {
        while (running_.load(std::memory_order_acquire)) {
            const int clientFd = accept(listenFd_, nullptr, nullptr);
            if (clientFd < 0) {
                if (!running_.load(std::memory_order_acquire)) { return; }
                continue;
            }
            Handle(clientFd);
            close(clientFd);
        }
    }

    void Handle(int clientFd)
    {
        timeval timeout{};
        timeout.tv_sec = 2;
        (void)setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        char request[kMaxRequestBytes + 1] = {};
        const auto received = recv(clientFd, request, kMaxRequestBytes, 0);
        if (received <= 0) { return; }
        request[received] = '\0';

        std::istringstream requestLine{std::string{request, static_cast<std::size_t>(received)}};
        std::string method;
        std::string path;
        std::string version;
        requestLine >> method >> path >> version;
        (void)version;

        std::string status;
        std::string contentType;
        std::string body;
        if (method != "GET") {
            status = "405 Method Not Allowed";
            contentType = "text/plain; charset=utf-8";
            body = "method not allowed\n";
        } else if (path == config_.metricsPath) {
            registry_.Add(Names::ExporterHttpRequests, 1.0);
            status = "200 OK";
            contentType = "text/plain; version=0.0.4; charset=utf-8";
            body = registry_.Render(config_.metricPrefix, config_.constantLabels);
        } else if (path == config_.healthPath) {
            status = "200 OK";
            contentType = "text/plain; charset=utf-8";
            body = "ok\n";
        } else {
            status = "404 Not Found";
            contentType = "text/plain; charset=utf-8";
            body = "not found\n";
        }

        std::ostringstream response;
        response << "HTTP/1.1 " << status << "\r\n"
                 << "Content-Type: " << contentType << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n\r\n"
                 << body;
        const auto text = response.str();
        std::size_t sent = 0;
        while (sent < text.size()) {
            const auto count = send(clientFd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) { return; }
            sent += static_cast<std::size_t>(count);
        }
    }

    ThreadBufferedMetricsCollector& registry_;
    StandaloneMetricsConfig config_;
    std::atomic<bool> running_{false};
    int listenFd_{-1};
    std::thread worker_;
};

class StandaloneMetricsBackend final : public MetricsBackend {
public:
    explicit StandaloneMetricsBackend(StandaloneMetricsConfig config)
        : config_(std::move(config))
    {}

    bool Start() override
    {
        std::map<std::string, MetricDescriptor> descriptors;
        for (auto& descriptor : DefaultAsuMetricDescriptors()) {
            descriptors[descriptor.name] = std::move(descriptor);
        }
        for (auto& descriptor : config_.descriptors) {
            descriptors[descriptor.name] = std::move(descriptor);
        }

        if (!config_.definitionPath.empty()) {
            std::vector<MetricDescriptor> configured;
            if (!LoadYamlDefinitions(config_.definitionPath, config_.metricPrefix, configured,
                                     error_)) {
                return false;
            }
            for (auto& descriptor : configured) {
                descriptors[descriptor.name] = std::move(descriptor);
            }
        }
        if (config_.metricsPath.empty() || config_.metricsPath.front() != '/' ||
            config_.healthPath.empty() || config_.healthPath.front() != '/') {
            error_ = "metrics and health paths must start with '/'";
            return false;
        }

        collector_ = std::make_unique<ThreadBufferedMetricsCollector>();
        for (const auto& item : descriptors) {
            if (!collector_->Register(item.second, error_)) {
                collector_.reset();
                return false;
            }
        }
        collector_->SetGaugeDirect(Names::ExporterUp, 1.0);
        collector_->StartAggregation(config_.aggregationIntervalMs);
        server_ = std::make_unique<MetricsHttpServer>(*collector_, config_);
        if (!server_->Start(error_)) {
            server_.reset();
            collector_->StopAggregation();
            collector_.reset();
            return false;
        }
        return true;
    }

    void Add(std::string_view name, double delta) noexcept override
    {
        if (collector_) { collector_->Add(name, delta); }
    }
    void Set(std::string_view name, double value) noexcept override
    {
        if (collector_) { collector_->Set(name, value); }
    }
    void Observe(std::string_view name, double value) noexcept override
    {
        if (collector_) { collector_->Observe(name, value); }
    }
    void Flush() override
    {
        if (collector_) { collector_->Flush(); }
    }
    void Stop() override
    {
        if (collector_) { collector_->SetGaugeDirect(Names::ExporterUp, 0.0); }
        if (server_) {
            server_->Stop();
            server_.reset();
        }
        if (collector_) {
            collector_->StopAggregation();
            collector_.reset();
        }
    }
    std::string LastError() const override { return error_; }

private:
    StandaloneMetricsConfig config_;
    std::unique_ptr<ThreadBufferedMetricsCollector> collector_;
    std::unique_ptr<MetricsHttpServer> server_;
    std::string error_;
};

}  // namespace

std::vector<MetricDescriptor> DefaultAsuMetricDescriptors()
{
    return {
        Counter(Names::QueryRequests, "Total ASU client query submissions"),
        Counter(Names::QueryEntries, "Total keys submitted to ASU client query"),
        Counter(Names::QueryErrors, "Total failed ASU client query submissions"),
        Histogram(Names::QuerySubmitDuration, "ASU client query submission duration in seconds"),
        Counter(Names::LoadRequests, "Total ASU client load submissions"),
        Counter(Names::LoadEntries, "Total entries submitted to ASU client load"),
        Counter(Names::LoadErrors, "Total failed ASU client load submissions"),
        Histogram(Names::LoadSubmitDuration, "ASU client load submission duration in seconds"),
        Counter(Names::StoreRequests, "Total ASU client store submissions"),
        Counter(Names::StoreEntries, "Total entries submitted to ASU client store"),
        Counter(Names::StoreErrors, "Total failed ASU client store submissions"),
        Histogram(Names::StoreSubmitDuration, "ASU client store submission duration in seconds"),
        Counter(Names::BatchLoadRequests, "Total ASU client batch-load submissions"),
        Counter(Names::BatchLoadEntries, "Total entries submitted to ASU client batch-load"),
        Counter(Names::BatchLoadErrors, "Total failed ASU client batch-load submissions"),
        Histogram(Names::BatchLoadSubmitDuration,
                  "ASU client batch-load submission duration in seconds"),
        Counter(Names::BatchStoreRequests, "Total ASU client batch-store submissions"),
        Counter(Names::BatchStoreEntries, "Total entries submitted to ASU client batch-store"),
        Counter(Names::BatchStoreErrors, "Total failed ASU client batch-store submissions"),
        Histogram(Names::BatchStoreSubmitDuration,
                  "ASU client batch-store submission duration in seconds"),
        Counter(Names::DeleteRequests, "Total ASU client delete submissions"),
        Counter(Names::DeleteEntries, "Total keys submitted to ASU client delete"),
        Counter(Names::DeleteErrors, "Total failed ASU client delete submissions"),
        Histogram(Names::DeleteSubmitDuration,
                  "ASU client delete submission duration in seconds"),
        Counter(Names::WaitRequests, "Total ASU client wait calls"),
        Counter(Names::WaitErrors, "Total failed ASU client wait calls"),
        Histogram(Names::WaitDuration, "ASU client wait duration in seconds"),
        Gauge(Names::ExporterUp, "Whether the ASU standalone metrics exporter is running"),
        Counter(Names::ExporterHttpRequests,
                "Total HTTP requests served by the ASU metrics endpoint"),
    };
}

std::shared_ptr<MetricsBackend> CreateStandaloneMetricsBackend(StandaloneMetricsConfig config)
{
    return std::make_shared<StandaloneMetricsBackend>(std::move(config));
}

}  // namespace UC::ASU::Metrics
