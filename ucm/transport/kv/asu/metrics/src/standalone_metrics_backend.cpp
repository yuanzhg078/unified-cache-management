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
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

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

class MetricsRegistry {
public:
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
        MetricState state;
        state.descriptor = descriptor;
        state.bucketCounts.assign(descriptor.buckets.size(), 0);
        states_[descriptor.name] = std::move(state);
        return true;
    }

    void Add(std::string_view name, double delta) noexcept
    {
        if (!std::isfinite(delta) || delta < 0.0) { return; }
        std::lock_guard<std::mutex> lock{mutex_};
        auto iter = states_.find(std::string{name});
        if (iter == states_.end() || iter->second.descriptor.type != MetricType::COUNTER) { return; }
        iter->second.value += delta;
    }

    void Set(std::string_view name, double value) noexcept
    {
        if (!std::isfinite(value)) { return; }
        std::lock_guard<std::mutex> lock{mutex_};
        auto iter = states_.find(std::string{name});
        if (iter == states_.end() || iter->second.descriptor.type != MetricType::GAUGE) { return; }
        iter->second.value = value;
    }

    void Observe(std::string_view name, double value) noexcept
    {
        if (!std::isfinite(value)) { return; }
        std::lock_guard<std::mutex> lock{mutex_};
        auto iter = states_.find(std::string{name});
        if (iter == states_.end() || iter->second.descriptor.type != MetricType::HISTOGRAM) {
            return;
        }
        auto& state = iter->second;
        state.sum += value;
        ++state.count;
        for (std::size_t index = 0; index < state.descriptor.buckets.size(); ++index) {
            if (value <= state.descriptor.buckets[index]) { ++state.bucketCounts[index]; }
        }
    }

    std::string Render(const std::string& prefix,
                       const std::map<std::string, std::string>& labels) const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        std::ostringstream output;
        output << std::setprecision(17);
        for (const auto& item : states_) {
            const auto& state = item.second;
            const auto fullName = prefix + state.descriptor.name;
            output << "# HELP " << fullName << ' '
                   << EscapeHelp(state.descriptor.documentation) << '\n';
            output << "# TYPE " << fullName << ' '
                   << PrometheusTypeName(state.descriptor.type) << '\n';
            if (state.descriptor.type == MetricType::HISTOGRAM) {
                for (std::size_t index = 0; index < state.descriptor.buckets.size(); ++index) {
                    output << fullName << "_bucket"
                           << RenderLabels(labels, "le",
                                           FormatNumber(state.descriptor.buckets[index]))
                           << ' ' << state.bucketCounts[index] << '\n';
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
        MetricDescriptor descriptor;
        double value{0.0};
        double sum{0.0};
        std::uint64_t count{0};
        std::vector<std::uint64_t> bucketCounts;
    };

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

    mutable std::mutex mutex_;
    std::map<std::string, MetricState> states_;
};

class MetricsHttpServer {
public:
    MetricsHttpServer(MetricsRegistry& registry, StandaloneMetricsConfig config)
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

    MetricsRegistry& registry_;
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

        for (const auto& item : descriptors) {
            if (!registry_.Register(item.second, error_)) { return false; }
        }
        server_ = std::make_unique<MetricsHttpServer>(registry_, config_);
        if (!server_->Start(error_)) {
            server_.reset();
            return false;
        }
        registry_.Set(Names::ExporterUp, 1.0);
        return true;
    }

    void Add(std::string_view name, double delta) noexcept override { registry_.Add(name, delta); }
    void Set(std::string_view name, double value) noexcept override { registry_.Set(name, value); }
    void Observe(std::string_view name, double value) noexcept override
    {
        registry_.Observe(name, value);
    }
    void Flush() override {}
    void Stop() override
    {
        registry_.Set(Names::ExporterUp, 0.0);
        if (server_) {
            server_->Stop();
            server_.reset();
        }
    }
    std::string LastError() const override { return error_; }

private:
    StandaloneMetricsConfig config_;
    MetricsRegistry registry_;
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
