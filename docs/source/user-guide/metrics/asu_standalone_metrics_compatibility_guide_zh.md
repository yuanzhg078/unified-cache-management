# ASU 独立运行时的 Metrics 框架与 UCM 兼容方案

> 本文保留为详细实现和演进记录。当前架构、时序与 standalone/UCM 兼容契约请以[《ASU Metrics 设计：Standalone 与 UCM 统一兼容方案》](asu_metrics_architecture_zh.md)为准。

## 1. 目标与结论

ASU 不经过 vLLM、由 `kv-test` 直接拉起时，是否复用 UCM 的 C++ metrics，取决于 `kv-test` 的构建和运行包是否允许携带 UCM 依赖：

| 约束 | 推荐方案 |
|---|---|
| 不经过 vLLM，但允许链接 UCM metrics | 复用 `UC::Metrics` 采集核心，新增独立 C++ exporter |
| `kv-test` 运行包完全不能带 UCM 代码、头文件或动态库 | 实现 ASU 自己的 metrics facade 和 standalone backend，再提供可选 UCM adapter |

第一种情况当前真正缺少的不是“埋点框架”，而是一个不依赖 vLLM/Python 的 Prometheus HTTP 导出器。第二种情况则需要独立的 C++ collector 和 exporter，但不应让 ASU 业务代码直接绑定这套实现。

建议最终形成一套埋点、两种导出入口：

```text
ASU client / transport / provider 中的 C++ 埋点
                  │
                  ▼
        进程内共享的 libucm_metrics.so
                  │
          ┌───────┴────────┐
          │                │
          ▼                ▼
vLLM/UCM 导出模式      kv-test 独立导出模式
Python ucmmetrics      C++ Prometheus exporter
PrometheusStatsLogger  HTTP :<port>/metrics
          │                │
          └───────┬────────┘
                  ▼
             Prometheus
                  │
                  ▼
               Grafana
```

这里需要特别澄清：

- Grafana 不直接抓 ASU，也不直接抓 `kv-test`。
- `kv-test` 或 vLLM 暴露 `/metrics`。
- Prometheus 定时抓取 `/metrics` 并保存时序数据。
- Grafana 查询 Prometheus，负责画图。
- 没有 Prometheus 时，仍可用 `curl http://127.0.0.1:<port>/metrics` 查看当前指标文本；只是没有长期存储、PromQL 和 Grafana 图表。

因此，ASU 独立运行并不意味着必须复制 UCM 的实现。若允许依赖 UCM，就直接复用 UCM metrics；若禁止依赖 UCM，更稳妥的结构是“ASU 自己的统一接口 + 两种后端”，让 standalone 和 UCM 场景共享同一套 ASU 埋点代码。

## 2. 当前代码实际是什么结构

本文基于 `D:\a_storage\3_debug\unified-cache-management` 当前代码分析。

### 2.1 C++ 采集核心

核心代码位于：

```text
ucm/shared/metrics/cc/api/metrics_api.h
ucm/shared/metrics/cc/api/metrics_api.cc
ucm/shared/metrics/cc/domain/metrics.h
ucm/shared/metrics/cc/domain/metrics.cc
```

它提供以下接口：

```cpp
SetUp(maxVectorLen);
CreateStats(name, type);
UpdateStats(name, value);
GetAllStatsAndClear();
```

其中：

- Counter 在每个线程的 buffer 中累加增量。
- Gauge 在 buffer 中保存最近一次设置值。
- Histogram 暂存原始观测样本。
- `GetAllStatsAndClear()` 切换双 buffer、合并所有线程的数据并清空已读取部分。

这个 C++ 核心本身不依赖 vLLM，因此 ASU 和 `kv-test` 场景都能使用。

### 2.2 当前 Python/Prometheus 导出层

现有导出代码主要位于：

```text
ucm/shared/metrics/cpy/metrics.py.cc
ucm/observability.py
ucm/integration/vllm/ucm_connector.py
```

数据链路是：

```text
C++ UpdateStats
    -> pybind 模块 ucmmetrics
    -> PrometheusStatsLogger 后台线程
    -> Python prometheus_client Counter/Gauge/Histogram
    -> vLLM 进程的 /metrics
```

`PrometheusStatsLogger` 读取 `metrics_configs.yaml`，注册指标并周期调用 `get_all_stats_and_clear()`。这部分依赖 Python 运行环境和 Prometheus Python client，`kv-test` 当前没有启动它。

### 2.3 `kv-test` 当前的运行方式

`kv-test` 是 C++ 可执行程序，入口位于：

```text
ucm/transport/kv/kv-test/src/kv_test_main.cpp
ucm/transport/kv/kv-test/src/kv_test_app.cpp
```

它通过下面的代码动态加载 ASU：

```text
ucm/transport/kv/kv-test/src/asu_runtime_proxy.cpp
```

动态库包括：

```text
libasu_transport.so
libasu_client.so
```

在本次改造之前，`kv-test` 没有 HTTP server，也没有链接 Prometheus C++ 库。其 bench 中已有的 `BenchMetrics` 只是压测结果结构，用来打印带宽、IOPS、时延和错误数，不等同于 UCM 的进程内 metrics。本次改造已经新增 ASU 自有的 `libasu_metrics.so` 和 standalone HTTP exporter。

## 3. 为什么当前直接加埋点后仍可能看不到

### 3.1 缺少独立 exporter

ASU 调用 `UpdateStats()` 只是在内存中产生指标。必须有消费者把数据转换为 Prometheus exposition format，并通过 HTTP 返回：

```text
# HELP ucm:asu_store_duration_seconds ASU store duration
# TYPE ucm:asu_store_duration_seconds histogram
ucm:asu_store_duration_seconds_bucket{le="0.001",source="kv-test"} 12
ucm:asu_store_duration_seconds_bucket{le="0.005",source="kv-test"} 26
ucm:asu_store_duration_seconds_bucket{le="+Inf",source="kv-test"} 30
ucm:asu_store_duration_seconds_sum{source="kv-test"} 0.074
ucm:asu_store_duration_seconds_count{source="kv-test"} 30
```

仅有 C++ 埋点，没有 exporter 时，`curl /metrics` 没有可访问的地址。

### 3.2 改造前 UCM metrics 被编译为 STATIC 库

原始分支的 `ucm/shared/metrics/CMakeLists.txt` 使用：

```cmake
add_library(metrics STATIC ...)
```

如果 `libasu_client.so`、`libasu_transport.so`、`kv-test` 和 Python `ucmmetrics` 各自静态链接一份 `metrics`，每个 ELF 模块中会有自己的 `Metrics::GetInstance()` 单例：

```text
ASU client 中的 Metrics singleton    A
ASU transport 中的 Metrics singleton B
kv-test exporter 中的 singleton      C
Python ucmmetrics 中的 singleton      D
```

ASU 把数据写进 A/B，而 exporter 读取 C/D，就会得到空数据。代码看起来都调用同一个 API，但运行时实际上不是同一个实例。

因此，独立 exporter 上线之前，必须先解决“同一进程只有一份 metrics collector”的问题。

### 3.3 `GetAllStatsAndClear()` 是破坏性读取

该接口读取后会清空已消费数据。假如同一进程同时运行两个消费者：

```text
Python vLLM exporter --------┐
                             ├──> GetAllStatsAndClear()
C++ standalone exporter -----┘
```

先读取的一方会拿走数据，后读取的一方可能读到空值。不能让两个 exporter 直接并发 drain 同一个 collector。

## 4. 推荐的目标架构

推荐把框架拆成四层。

### 4.1 Instrumentation：ASU 埋点层

ASU 只负责调用统一 API，不关心 Prometheus、HTTP、vLLM 或 Grafana：

```cpp
UC::Metrics::UpdateStats("asu_store_requests_total", 1.0);
UC::Metrics::UpdateStats("asu_store_duration_seconds", elapsedSeconds);
```

埋点名称、单位和类型在 vLLM 与 standalone 模式下必须一致。

### 4.2 Collector：进程内采集层

Collector 继续负责：

- 多线程低成本写入。
- Counter/Gauge/Histogram 分类。
- 聚合各线程 buffer。
- 返回一段时间内的增量或样本。

该层应编译成一份共享库，例如：

```text
libucm_metrics.so
```

所有生产者和消费者链接同一个 SONAME，从而共享同一个 `Metrics` 单例。

### 4.3 Dispatcher/Aggregator：单一 drain 与累积层

建议新增一个唯一的后台 drain 线程：

```text
Metrics collector
       │
       │ GetAllStatsAndClear()，仅这里调用
       ▼
MetricsDispatcher
       │
       ├── 更新 Counter 累计值
       ├── 更新 Gauge 最新值
       ├── 更新 Histogram bucket/sum/count
       └── 生成只读快照
```

Exporter 不再直接调用 `GetAllStatsAndClear()`，只读取 Dispatcher 的累计快照。这样可避免多消费者抢数据。

如果第一阶段只想做最小改造，也可以先规定“一个进程只能启用一个 exporter”：

- vLLM 进程启用 Python exporter。
- `kv-test` 进程启用 C++ exporter。

这在当前部署方式下一般够用，但长期建议仍迁移到 Dispatcher。

### 4.4 Exporter：输出层

提供两个 adapter：

```text
PythonPrometheusAdapter   保持当前 vLLM 路径兼容
StandaloneHttpExporter   给 kv-test 或其他纯 C++ 宿主使用
```

二者使用同一份 metric descriptor 和同一命名规则。

## 5. 共享库改造是第一优先级

### 5.1 建议的 CMake 结构

不要让 ASU 各组件静态嵌入自己的 metrics。建议改为：

```cmake
add_library(ucm_metrics SHARED ${UCMMETRICS_CC_SOURCE_FILES})

target_include_directories(ucm_metrics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cc/api
    ${CMAKE_CURRENT_SOURCE_DIR}/cc/domain
)

set_target_properties(ucm_metrics PROPERTIES
    OUTPUT_NAME ucm_metrics
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

target_link_libraries(ucmmetrics PRIVATE ucm_metrics)
target_link_libraries(asu_client PRIVATE ucm_metrics)
target_link_libraries(asu_transport PRIVATE ucm_metrics)
target_link_libraries(kv-test PRIVATE ucm_metrics)
```

还需要安装 `libucm_metrics.so`，并正确配置安装目录、`RPATH` 或 `LD_LIBRARY_PATH`。

### 5.2 动态加载场景为什么能共享

`kv-test` 使用 `RTLD_NOW | RTLD_GLOBAL` 加载 ASU 动态库。只要：

- `kv-test`、`libasu_client.so`、`libasu_transport.so` 和 `ucmmetrics` 都依赖同一个 `libucm_metrics.so.1`；
- 加载到的是同一路径/同 SONAME 的库；
- ASU 内部没有再静态打包另一份 collector；

动态链接器通常会在进程内复用这份共享库，因此埋点和 exporter 能看到同一个 singleton。

应通过以下手段验证，而不是只看 CMake：

```bash
ldd ./kv-test | grep ucm_metrics
ldd ./libasu_client.so | grep ucm_metrics
ldd ./libasu_transport.so | grep ucm_metrics
ldd ./ucmmetrics*.so | grep ucm_metrics
```

必要时可使用：

```bash
LD_DEBUG=libs ./kv-test bench ... 2>&1 | grep ucm_metrics
```

验收标准是同一进程只加载一份 `libucm_metrics.so.1`。

## 6. Standalone Prometheus exporter 怎么实现

### 6.1 推荐新增的代码目录

可以在共享 metrics 下新增：

```text
ucm/shared/metrics/cc/exporter/
├── metric_descriptor.h
├── metrics_dispatcher.h
├── metrics_dispatcher.cc
├── prometheus_snapshot.h
├── prometheus_text_serializer.h
├── prometheus_text_serializer.cc
├── standalone_metrics_server.h
└── standalone_metrics_server.cc
```

职责建议如下：

| 组件 | 职责 |
|---|---|
| `MetricDescriptor` | 保存 name、type、documentation、buckets 等元数据 |
| `MetricsDispatcher` | 唯一调用 `GetAllStatsAndClear()`，维护累计快照 |
| `PrometheusTextSerializer` | 把累计快照序列化为 Prometheus 文本格式 |
| `StandaloneMetricsServer` | 监听地址/端口，处理 `GET /metrics` 和健康检查 |

ASU 不应该依赖这些 exporter 头文件；ASU 只依赖 metrics API。

### 6.2 HTTP 实现选择

当前仓库未发现 prometheus-cpp、yaml-cpp 或现成 HTTP server 依赖，有两种可行选择。

#### 方案 A：引入 prometheus-cpp

优点：

- Counter、Gauge、Histogram 和文本格式由成熟库处理。
- HTTP exposer、escaping 和协议兼容问题较少。
- 后续扩展 labels 更方便。

缺点：

- 新增三方依赖和构建/安装成本。
- 要协调离线构建、ARM/Ascend 环境和包版本。
- 现有 collector 仍需适配到 prometheus-cpp 对象。

适合依赖治理比较完善的长期方案。

#### 方案 B：实现一个小型内部 HTTP exporter

只支持：

- `GET /metrics`
- `GET /health`
- Prometheus text format 0.0.4
- 固定连接上限、请求大小和超时

优点是依赖少，适合 `kv-test`；缺点是必须正确处理指标名、label escaping、Histogram、并发、socket 退出和异常输入。

若选择此方案，不要在 ASU 各模块里各写一份 HTTP server，应把它作为 `ucm_metrics_exporter` 公共库。

### 6.3 Counter/Gauge/Histogram 的累积规则

现有 collector 返回的是 drain 周期内的数据，而 Prometheus endpoint 必须暴露可持续抓取的当前状态。

Dispatcher 应按以下规则处理：

```text
Counter:
    exported_total[name] += drained_delta[name]

Gauge:
    exported_value[name] = drained_latest_value[name]

Histogram:
    对每个 drained sample：
        sum += sample
        count += 1
        对所有 sample <= upper_bound 的 bucket 累加
```

Histogram 输出必须包含：

```text
<name>_bucket{le="..."}
<name>_bucket{le="+Inf"}
<name>_sum
<name>_count
```

bucket 值是累计 bucket，不是互斥区间计数。

### 6.4 HTTP 请求不应触发 destructive drain

不要在每次 `GET /metrics` 时直接调用 `GetAllStatsAndClear()`。原因包括：

- curl 和 Prometheus 同时访问会相互影响。
- 两次抓取间隔不一致会改变观测行为。
- HTTP 请求线程会承担聚合开销。

正确方式是：

```text
后台线程每 N ms drain collector -> 更新锁保护的累计快照
HTTP 请求 -> 只复制/读取累计快照 -> 序列化返回
```

## 7. 如何兼容现有 `metrics_configs.yaml`

现有配置文件是：

```text
examples/metrics/metrics_configs.yaml
```

它目前负责定义：

- `metric_prefix`
- `log_interval`
- `histogram_max_length`
- 启用哪些 Counter/Gauge/Histogram
- documentation
- Histogram buckets
- Python Prometheus multiprocess 配置

Standalone 模式应继续把这份配置当作指标定义来源，而不是另起一套不同名字的 C++ 配置。

### 7.1 建议把配置拆成公共字段和 exporter 专属字段

例如：

```yaml
metric_prefix: "ucm:"
histogram_max_length: 10000

counter:
  - name: "asu_store_requests_total"
    documentation: "Total number of ASU store requests"

histogram:
  - name: "asu_store_duration_seconds"
    documentation: "ASU store duration in seconds"
    buckets: [0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1]

python_exporter:
  log_interval: 5
  multiproc_dir: "/vllm-workspace"

standalone_exporter:
  enabled: true
  listen_address: "127.0.0.1"
  port: 9108
  path: "/metrics"
  drain_interval_ms: 1000
  shutdown_grace_ms: 15000
```

为了不破坏已有文件，代码也可以先兼容顶层旧字段：顶层 `log_interval` 和 `multiproc_dir` 继续供 Python 使用，新增 `standalone_exporter` 块。

### 7.2 C++ 如何读取同一份 YAML

有三种选择：

1. 引入 `yaml-cpp`，运行时读取同一份 YAML。配置可动态修改，兼容性最好，但增加依赖。
2. 构建时用 Python 脚本把 YAML 生成 C++ descriptor 表。没有运行时 YAML 依赖，但修改配置后需要重新构建。
3. 为 standalone 单独写一份 key-value 配置，同时 metric descriptor 编译进 C++。实现快，但容易让两套配置漂移，不建议作为最终方案。

优先推荐第 1 种；如果部署环境严格限制三方库，则选第 2 种。

### 7.3 名称与 labels 兼容规则

要让现有 PromQL/Grafana 面板尽量复用：

- 保持 `metric_prefix: "ucm:"`。
- 同一含义的 metric 保持相同 name、type 和单位。
- Histogram 保持相同 buckets。
- 不在 ASU 埋点层写死 `model_name`、`worker_id` 等宿主标签。
- labels 由 exporter 添加。

当前 Python exporter 添加：

```text
model_name
worker_id
```

Standalone exporter 若要完全复用依赖这些标签的查询，可提供稳定默认值：

```text
model_name="standalone"
worker_id="asu-0"
source="kv-test"
```

不要默认把 PID 当 `worker_id`，否则每次启动都会产生新时序。Prometheus 自己还会添加 `job` 和 `instance` 标签。

Grafana 查询应尽量允许 `model_name="standalone"`，或者变量默认选 `All`。

## 8. `kv-test` 中具体改哪些位置

### 8.1 配置结构

在：

```text
ucm/transport/kv/kv-test/include/kv_test/kv_test_types.h
```

新增：

```cpp
struct MetricsServerConfig {
    bool enabled{false};
    std::string configPath;
    std::string listenAddress{"127.0.0.1"};
    std::uint16_t port{9108};
    std::string path{"/metrics"};
    std::string source{"kv-test"};
    std::string modelName{"standalone"};
    std::string workerId{"asu-0"};
    std::uint32_t drainIntervalMs{1000};
    std::uint32_t shutdownGraceMs{0};
};

struct KvTestConfig {
    // existing fields...
    MetricsServerConfig metrics;
};
```

### 8.2 配置加载

在：

```text
ucm/transport/kv/kv-test/src/kv_test_config_loader.cpp
```

读取以下配置项：

```ini
metrics.enabled=true
metrics.config_path=/opt/ucm/examples/metrics/metrics_configs.yaml
metrics.listen_address=127.0.0.1
metrics.port=9108
metrics.path=/metrics
metrics.source=kv-test
metrics.model_name=standalone
metrics.worker_id=asu-0
metrics.aggregation_interval_ms=500
metrics.shutdown_grace_ms=15000
```

这里的 key-value 文件是 `kv-test` 自身配置；`metrics.config_path` 再指向公共 YAML 指标目录。这样不用立即把整个 `kv-test` 配置格式迁移成 YAML。

还要增加 bool、uint16 范围和端口冲突校验。

### 8.3 启停位置

在：

```text
ucm/transport/kv/kv-test/src/kv_test_app.cpp
```

建议生命周期为：

```cpp
// 1. configLoader_.Load / MergeCommandOptions 完成
StandaloneMetricsServer metricsServer;
status = metricsServer.Start(config.metrics);

// 2. 创建 ASU client、Init、执行命令
status = clientRunner.Init(config);
status = RunCommand(...);

// 3. 关闭 ASU，使最后的 completion/cleanup 指标进入 collector
clientRunner.Shutdown();

// 4. 强制最后一次 drain，按配置保留一段抓取窗口，然后退出
metricsServer.Flush();
metricsServer.WaitForGracePeriod();
metricsServer.Stop();
```

`CONFIG_CHECK` 可只校验 metrics 配置，不必监听端口。`VERSION`、`--help` 不启动 exporter。

### 8.4 构建目标

在：

```text
ucm/transport/kv/kv-test/CMakeLists.txt
```

新增共享库链接：

```cmake
target_link_libraries(kv-test PRIVATE
    ${CMAKE_DL_LIBS}
    asu_ascend_deps
    ucm_metrics
    ucm_metrics_exporter
)
```

如果 exporter 自己已经 `PUBLIC` 链接 `ucm_metrics`，`kv-test` 不必重复声明，但 ASU client/transport 仍必须显式链接同一共享核心。

## 9. 短命令是 standalone scrape 的关键限制

`kv-test store`、`retrieve`、`exist` 等命令可能几百毫秒内结束。Prometheus 默认常见抓取间隔是 15 秒，因此 exporter 即使正确启动，也可能在第一次 scrape 前进程就退出。

例如：

```text
t=0.0s  kv-test 启动
t=0.2s  store 完成
t=0.3s  进程退出
t=8.0s  Prometheus 尝试抓取，目标已经不存在
```

推荐按使用场景处理。

### 9.1 bench 场景

`kv-test bench --duration 300` 是最适合 pull 模式的场景。进程长时间存活，Prometheus 可以持续抓取。

### 9.2 单次命令场景

提供一个可选的退出保留时间：

```ini
metrics.shutdown_grace_ms=15000
```

命令完成后先 `Flush()`，HTTP server 继续存活一段时间，至少允许 Prometheus 抓取一次。默认值是否为 0 应结合自动化测试时延决定；建议只在启用 metrics 时显式配置。

### 9.3 长期服务模式

如果需要持续观测多轮手工命令，最好新增 `kv-test serve` 或 daemon/bench 服务模式，让同一进程持续接收并执行请求。这样比每条命令启动一个短生命周期 exporter 更符合 Prometheus pull 模型。

### 9.4 Pushgateway 是否需要

Pushgateway 适合短生命周期批任务，但它改变了数据链路和指标生命周期管理，还可能保留已退出任务的陈旧指标。它可以作为单次命令的补充方案，不应替代 ASU 服务/bench 的标准 `/metrics` pull 模式。

## 10. 两种运行模式如何共存

### 10.1 vLLM 模式

保持现有行为：

```text
ASU/UCM C++ UpdateStats
    -> libucm_metrics.so
    -> ucmmetrics pybind
    -> PrometheusStatsLogger
    -> vLLM /metrics
```

默认不要再在相同进程启动 standalone server，避免端口重复和双 exporter drain。

### 10.2 kv-test 模式

```text
ASU C++ UpdateStats
    -> libucm_metrics.so
    -> MetricsDispatcher
    -> StandaloneMetricsServer
    -> kv-test :9108/metrics
```

不需要安装 vLLM，也不需要 Python Prometheus client。

### 10.3 其他纯 C++ 宿主

未来其他进程也可以复用：

```cpp
StandaloneMetricsServer server;
server.Start(config);

// 创建并使用 ASU

server.Flush();
server.Stop();
```

因此 exporter 应属于 `ucm/shared/metrics`，而不是写死在 `kv-test` 目录。

## 11. Prometheus 与 Grafana 配置示例

### 11.1 先用 curl 验证

运行长时间 bench：

```bash
./kv-test bench store --duration 300 --configpath ./asu_kv_test.conf
```

另一个终端检查：

```bash
curl -s http://127.0.0.1:9108/metrics | grep '^ucm:'
```

这一步不需要 Prometheus。

### 11.2 Prometheus 抓取配置

```yaml
scrape_configs:
  - job_name: ucm-asu-kv-test
    scrape_interval: 5s
    static_configs:
      - targets:
          - 10.0.0.12:9108
```

检查 Prometheus Targets 页面，目标应为 `UP`。

### 11.3 Grafana

Grafana datasource 指向 Prometheus，然后使用与现有指标同名的 PromQL。例如：

```promql
rate({__name__="ucm:asu_store_requests_total"}[1m])
```

Histogram P99 示例：

```promql
histogram_quantile(
  0.99,
  sum by (le, instance) (
    rate({__name__="ucm:asu_store_duration_seconds_bucket"}[5m])
  )
)
```

若已有 Grafana JSON 只展示 connector/pipeline 的旧指标，它不会因为 `metrics_configs.yaml` 新增 ASU 指标就自动产生新面板。配置文件决定“注册和采集什么”，Grafana JSON 决定“查询并展示什么”。需要为新的 `ucm:asu_*` 指标新增或修改 panel。

## 12. 推荐的实施顺序

### 阶段 1：打通最小闭环

1. 将 metrics core 改为共享库。
2. ASU client 和 transport 链接同一共享库。
3. 在 ASU 的一条 store/retrieve 路径增加少量测试指标。
4. 给 `kv-test` 加一个最小 standalone exporter。
5. 用长时间 bench 和 curl 验证。

验收标准：

```bash
curl -s http://127.0.0.1:9108/metrics | grep '^ucm:asu_'
```

能看到非零且持续变化的数据。

### 阶段 2：兼容配置与完整语义

1. C++ 读取或生成同一份 YAML metric descriptor。
2. 完整实现 HELP、TYPE、labels 和 Histogram。
3. 添加 Counter 单调性、Gauge 更新、Histogram bucket 测试。
4. 增加 `Flush()` 和短命令 grace period。

### 阶段 3：统一消费模型

1. 引入 `MetricsDispatcher`，保证只有一个 destructive drain。
2. Python exporter 改为读取 dispatcher 快照或通过订阅 adapter 获取数据。
3. 验证 vLLM 与 standalone 两种模式的兼容性。

### 阶段 4：Grafana 与运维

1. 增加 ASU overview/transport/store 面板。
2. 配置 job、instance、model_name、worker_id 变量兼容 standalone。
3. 增加 exporter 自监控指标。
4. 补齐端口、绑定地址、资源上限和关闭流程文档。

## 13. 建议补充的 exporter 自监控指标

Exporter 自己也应暴露少量健康指标：

```text
ucm:metrics_exporter_up
ucm:metrics_last_drain_timestamp_seconds
ucm:metrics_drain_errors_total
ucm:metrics_dropped_histogram_samples_total
ucm:metrics_registered_series
```

这些指标可以区分：

- ASU 没有业务流量。
- ASU 有埋点但 exporter 没有成功 drain。
- Histogram buffer 已满并丢样本。
- Prometheus 没抓到 endpoint。

## 14. 测试矩阵

| 场景 | 需要验证的结果 |
|---|---|
| kv-test bench + curl | `/metrics` 可访问，指标持续增长 |
| kv-test 短 store + grace period | 命令完成后仍能抓到最终值 |
| Counter | 多次 scrape 不回退，重启后新进程可从 0 开始 |
| Gauge | 输出最后一次有效值 |
| Histogram | bucket、sum、count 与输入样本一致 |
| 多线程 ASU | 不崩溃、不死锁，聚合总数正确 |
| client + transport 同时埋点 | exporter 能同时看到两边指标 |
| vLLM 模式 | 原有 `ucm:` 指标仍可从 vLLM `/metrics` 获取 |
| 禁用 standalone | 不监听端口，不启动后台线程 |
| 端口被占用 | 明确失败或按策略禁用，不能静默丢指标 |
| 配置缺失/非法 | `config check` 给出明确错误 |
| 进程退出 | drain/server 线程正常 join，不悬挂 |

还应增加一个专门的共享实例测试：ASU 动态库内调用 `UpdateStats("asu_link_test_total", 1)`，`kv-test` exporter 必须能读取到 1。这个测试能直接发现错误的静态链接或重复 singleton。

## 15. 风险和边界

### 15.1 指标基数

不要把 key、request_id、错误文本、线程 ID、连接 ID 等无限变化值作为 label。应使用有限枚举，如 operation、result、transport_type、store_type。

### 15.2 Histogram 内存

当前实现保存原始样本，并受 `histogram_max_length` 限制。如果 drain 不及时，高流量场景会达到上限并丢弃样本。短期应缩短 drain interval 并记录 dropped 指标；长期可考虑在采集侧直接维护 bucket/count/sum，避免保存全部原始样本。

### 15.3 网络暴露

`/metrics` 通常无认证。`kv-test` 默认建议绑定 `127.0.0.1`；跨机器抓取时显式改为业务网卡地址，并由防火墙限制来源。不要默认监听所有公网接口。

### 15.4 fork 与多进程

进程内 singleton 和后台线程不适合在启动后再 fork。若宿主采用多进程模式，应在每个子进程完成 fork 后分别初始化，或使用独立端口/Prometheus multiprocess 方案。`kv-test` 当前主要是单进程多线程，优先覆盖这一模式。

## 16. ASU 统一接口与两种后端的实现设计

当 `kv-test` 运行包不能包含任何 UCM 依赖时，推荐采用下面的结构，而不是把 `UC::Metrics` 的代码直接复制到 ASU。

```text
ASU client / transport / provider
              │
              │ 只调用 ASU::Metrics 公共接口
              ▼
       libasu_metrics_api
              │
      ┌───────┴─────────┐
      │                 │
      ▼                 ▼
standalone backend    UCM backend
独立 collector        UcmMetricsAdapter
+ dispatcher          转发到 UC::Metrics
+ HTTP exporter       不启动自己的 HTTP server
      │                 │
      ▼                 ▼
kv-test :9108         vLLM /metrics
```

### 16.1 设计目标

这套设计需要同时满足：

1. ASU 业务埋点只写一次。
2. standalone 构建不包含 UCM 头文件、库或 Python。
3. UCM 构建不重复维护另一套 ASU 埋点。
4. 每个进程只激活一个 metrics backend。
5. 两种模式输出相同的指标名称、类型、单位和 buckets。
6. Grafana/PromQL 不关心指标来自 `kv-test` 还是 vLLM。

### 16.2 推荐目录结构

可以在 ASU 下新增：

```text
ucm/transport/kv/asu/metrics/
├── CMakeLists.txt
├── include/asu/metrics/
│   ├── metrics.h
│   ├── metrics_backend.h
│   ├── metrics_config.h
│   └── metric_names.h
├── api/
│   └── metrics.cc
├── standalone/
│   ├── standalone_metrics_backend.h
│   ├── standalone_metrics_backend.cc
│   ├── metrics_registry.h
│   ├── metrics_registry.cc
│   ├── metrics_dispatcher.h
│   ├── metrics_dispatcher.cc
│   ├── prometheus_text_serializer.h
│   ├── prometheus_text_serializer.cc
│   ├── metrics_http_server.h
│   └── metrics_http_server.cc
├── ucm_adapter/
│   ├── ucm_metrics_backend.h
│   └── ucm_metrics_backend.cc
└── test/
    ├── metrics_api_test.cc
    ├── standalone_backend_test.cc
    ├── prometheus_serializer_test.cc
    └── ucm_adapter_test.cc
```

目录含义：

- `include` 和 `api` 是 ASU 所有构建都可见的稳定接口。
- `standalone` 是完全独立的 collector 和 exporter，不依赖 UCM。
- `ucm_adapter` 只在 UCM 构建中编译，可以包含 UCM metrics 头文件。
- ASU client/transport/provider 不能直接 include `standalone` 或 `ucm_adapter` 的头文件。

### 16.3 ASU 公共 facade

建议公共接口只暴露 ASU 自己的命名空间和类型：

下面示例为了突出层次简写为 `ASU::Metrics`。当前仓库的 ASU 类型实际位于 `UC::ASU`，正式落地时建议使用 `UC::ASU::Metrics`，与现有代码命名空间保持一致；无论采用哪个名字，公共接口中都不能出现 `UC::Metrics`。

```cpp
// include/asu/metrics/metrics.h
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace ASU::Metrics {

class MetricsBackend;
struct MetricsConfig;

bool Initialize(const MetricsConfig& config,
                std::unique_ptr<MetricsBackend> backend);

void Shutdown();
void Flush();

void Add(std::string_view name, double delta = 1.0);
void Set(std::string_view name, double value);
void Observe(std::string_view name, double value);

bool IsEnabled();

}  // namespace ASU::Metrics
```

公共头文件中不能出现：

```cpp
#include "metrics_api.h"       // UCM header
UC::Metrics::UpdateStats(...); // UCM namespace
```

否则 standalone 构建仍然在源码层依赖 UCM。

Facade 内部持有当前 backend：

```cpp
// api/metrics.cc
namespace {
std::mutex gBackendMutex;
std::unique_ptr<ASU::Metrics::MetricsBackend> gBackend;
}

void ASU::Metrics::Add(std::string_view name, double delta)
{
    auto* backend = GetBackend();
    if (backend != nullptr) { backend->Add(name, delta); }
}
```

高频路径中不应每次获取全局互斥锁。实际实现可使用启动阶段固定 backend、原子指针或进程生命周期内不替换的对象。`Shutdown()` 前必须先停止 ASU 业务线程，避免 backend 被释放时仍有写入。

### 16.4 Backend 抽象接口

```cpp
// include/asu/metrics/metrics_backend.h
#pragma once

#include <string_view>

namespace ASU::Metrics {

class MetricsBackend {
public:
    virtual ~MetricsBackend() = default;

    virtual bool Start() = 0;
    virtual void Add(std::string_view name, double delta) noexcept = 0;
    virtual void Set(std::string_view name, double value) noexcept = 0;
    virtual void Observe(std::string_view name, double value) noexcept = 0;
    virtual void Flush() = 0;
    virtual void Stop() = 0;
};

}  // namespace ASU::Metrics
```

埋点失败不能破坏 KV 主业务，因此 `Add/Set/Observe` 应是 `noexcept` 语义：

- 未初始化时静默跳过或只记录一次告警。
- 未注册指标时忽略并增加内部错误计数。
- Histogram buffer 满时丢弃样本并增加 dropped counter。
- exporter 端口失败由初始化阶段报告，不在业务线程抛异常。

### 16.5 统一指标名称

不要在多个 `.cc` 中手写字符串，建议集中定义：

```cpp
// include/asu/metrics/metric_names.h
namespace ASU::Metrics::Names {

inline constexpr std::string_view StoreRequests =
    "asu_store_requests_total";
inline constexpr std::string_view StoreErrors =
    "asu_store_errors_total";
inline constexpr std::string_view StoreDuration =
    "asu_store_duration_seconds";
inline constexpr std::string_view RetrieveRequests =
    "asu_retrieve_requests_total";
inline constexpr std::string_view RetrieveDuration =
    "asu_retrieve_duration_seconds";

}  // namespace ASU::Metrics::Names
```

ASU 埋点示例：

```cpp
using namespace ASU::Metrics;

Add(Names::StoreRequests);
const auto begin = std::chrono::steady_clock::now();

auto status = DoStore(request);

const auto seconds = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - begin).count();
Observe(Names::StoreDuration, seconds);

if (!status.ok()) { Add(Names::StoreErrors); }
```

Facade 中使用不带 `ucm:` 的内部名称，exporter 再加 `metric_prefix`。这与当前 Python `PrometheusStatsLogger` 的行为一致，可以避免重复生成 `ucm:ucm:*`。

### 16.6 Standalone backend

Standalone backend 负责完整的纯 C++ 路径：

```text
ASU::Metrics::Add/Set/Observe
        │
        ▼
StandaloneMetricsBackend
        │
        ▼
ThreadBufferedMetricsCollector（每线程双 buffer）
        │
        ▼
MetricsDispatcher（定时聚合）
        │
        ▼
PrometheusSnapshot（累计快照）
        │
        ▼
MetricsHttpServer GET /metrics
```

可以定义：

```cpp
class StandaloneMetricsBackend final : public MetricsBackend {
public:
    explicit StandaloneMetricsBackend(StandaloneMetricsConfig config);

    bool Start() override;
    void Add(std::string_view name, double delta) noexcept override;
    void Set(std::string_view name, double value) noexcept override;
    void Observe(std::string_view name, double value) noexcept override;
    void Flush() override;
    void Stop() override;

private:
    ThreadBufferedMetricsCollector collector_;
    MetricsHttpServer httpServer_;
};
```

初始化时由 backend：

1. 读取 metric descriptors。
2. 注册 Counter/Gauge/Histogram。
3. 启动 dispatcher 线程。
4. 启动 HTTP server。
5. 将 `/metrics` 请求映射到只读 snapshot。

`kv-test` 只负责创建和销毁 backend，不应该直接实现 registry 或 Prometheus 序列化。

当前落地代码已采用上述模型：`Add/Set/Observe` 写入调用线程自己的双 buffer；聚合线程按 `metrics.aggregation_interval_ms`（默认 500 ms）翻转 buffer、合并增量到累计 snapshot；HTTP 线程只读 snapshot。`Flush()` 会同步执行一次聚合，因此 `kv-test` 在命令结束前调用它，最终值无需等待下一个周期。该 collector 独立于 UCM，不会调用 `GetAllStatsAndClear()`。

ASU client 的高频内建指标（Query/Load/Store/Batch/Wait）使用固定 `MetricId` 和 batch 写入：一次提交只获取一次 backend、一次 TLS buffer、一次 slot lock，并在 metrics 未启用时跳过时钟读取。`Add(std::string_view, ...)` 仍保留给与 UCM 一致的动态/自定义指标；两种入口最终写入同一个 collector。

内建指标的名称和类型是固定契约。YAML 可以覆盖它们的说明和 Histogram bucket，但不能把 Counter/Gauge/Histogram 改成另一种类型；standalone 启动会拒绝此类配置，避免静默改变 ASU client 的累计语义。

### 16.7 UCM backend adapter

UCM backend 不保存第二份数据，只把调用转发给现有 UCM 框架：

```cpp
// ucm_adapter/ucm_metrics_backend.cc
#include "asu/metrics/metrics_backend.h"
#include "metrics_api.h"  // 仅这个可选 target 依赖 UCM

namespace ASU::Metrics {

class UcmMetricsBackend final : public MetricsBackend {
public:
    bool Start() override
    {
        // SetUp/CreateStats 可由 UCM 宿主统一负责；
        // 如果由 adapter 负责，必须保证只初始化一次。
        return true;
    }

    void Add(std::string_view name, double delta) noexcept override
    {
        UC::Metrics::UpdateStats(std::string{name}, delta);
    }

    void Set(std::string_view name, double value) noexcept override
    {
        UC::Metrics::UpdateStats(std::string{name}, value);
    }

    void Observe(std::string_view name, double value) noexcept override
    {
        UC::Metrics::UpdateStats(std::string{name}, value);
    }

    void Flush() override {}
    void Stop() override {}
};

}  // namespace ASU::Metrics
```

UCM adapter 模式下：

- 不启动 ASU 自己的 HTTP server。
- 不调用 ASU standalone collector。
- 不直接调用 `GetAllStatsAndClear()`。
- 继续由 UCM/Python/vLLM 路径负责导出。

当前 `UC::Metrics::UpdateStats` 根据注册时的类型决定 Add/Set/Observe 语义，因此 descriptor 中的类型必须与 UCM `CreateStats` 注册类型完全一致。

### 16.8 Backend 的选择方式

建议以编译时选择为主、运行时配置为辅。

```cmake
set(ASU_METRICS_BACKEND "standalone" CACHE STRING
    "ASU metrics backend: standalone, ucm, or none")

if(ASU_METRICS_BACKEND STREQUAL "standalone")
    add_library(asu_metrics_backend ...standalone sources...)
    target_compile_definitions(asu_metrics_backend
        PRIVATE ASU_METRICS_STANDALONE=1)
elseif(ASU_METRICS_BACKEND STREQUAL "ucm")
    add_library(asu_metrics_backend ...ucm adapter sources...)
    target_link_libraries(asu_metrics_backend PRIVATE ucm_metrics)
    target_compile_definitions(asu_metrics_backend
        PRIVATE ASU_METRICS_UCM=1)
elseif(ASU_METRICS_BACKEND STREQUAL "none")
    add_library(asu_metrics_backend ...noop backend source...)
else()
    message(FATAL_ERROR "Unknown ASU_METRICS_BACKEND")
endif()
```

建议产物关系：

```text
Standalone 发布包：
    kv-test
    libasu_client.so
    libasu_transport.so
    libasu_metrics.so
    不包含 libucm_metrics.so

UCM 发布包：
    libasu_client.so
    libasu_transport.so
    libasu_metrics_ucm_adapter.so（也可静态进入 ASU DSO）
    libucm_metrics.so
    ucmmetrics Python module
```

如果同一份预编译 `libasu_client.so` 必须同时在两种环境运行，可以进一步采用 backend factory/plugin：ASU core 只依赖接口，在宿主启动时注入 standalone 或 UCM backend。但这会增加 ABI、动态库生命周期和版本管理成本；如果能够分别构建两个发布包，优先使用编译时选择。

### 16.9 Backend 由谁创建

推荐让宿主决定后端，ASU client 只消费已经安装的 facade。

`kv-test` 中：

```cpp
auto backend = std::make_unique<StandaloneMetricsBackend>(metricsConfig);
if (!ASU::Metrics::Initialize(metricsConfig, std::move(backend))) {
    return MetricsInitializationError();
}

// 创建 ASU client、执行命令

ASU::Metrics::Flush();
ASU::Metrics::Shutdown();
```

UCM connector/宿主中：

```cpp
auto backend = std::make_unique<UcmMetricsBackend>();
ASU::Metrics::Initialize(metricsConfig, std::move(backend));
```

如果 Python 是最外层宿主，可以在 ASU client 初始化入口中根据编译模式创建 adapter，但必须明确谁拥有初始化与关闭责任，避免 client、transport 各初始化一次。

### 16.10 配置与 descriptor 的兼容

建议把“指标定义”和“exporter 运行参数”区分开：

```cpp
enum class MetricType { Counter, Gauge, Histogram };

struct MetricDescriptor {
    std::string name;
    MetricType type;
    std::string help;
    std::vector<double> buckets;
};

struct MetricsConfig {
    bool enabled{false};
    std::string prefix{"ucm:"};
    std::vector<MetricDescriptor> descriptors;
};
```

两种 backend 使用同一组 descriptor：

- standalone backend 用它创建自己的 registry 和 histogram buckets。
- UCM 模式用它调用 `SetUp/CreateStats`，或校验 UCM Python 已注册的定义。

如果 UCM Python exporter 仍读取 `metrics_configs.yaml`，ASU standalone 最好也从同一文件读取或由同一文件生成 C++ descriptor。不能在两个地方手工维护不同 buckets。

### 16.11 Labels 的处理位置

ASU 当前 `UC::Metrics` API 不接受 labels，因此不要在 facade 中设计每次调用携带任意动态 labels 的接口，否则 UCM adapter 无法等价映射。

第一阶段建议：

- operation 使用不同指标名或少量固定 descriptor。
- `model_name`、`worker_id`、`source` 等宿主标签由 exporter 统一添加。
- 禁止 key、request_id、线程 ID 等高基数标签。

如果未来扩展 UCM metrics API 支持 labels，再同步扩展 ASU backend 接口。

### 16.12 避免同时启用两个后端

初始化函数必须拒绝重复安装：

```cpp
if (gBackend != nullptr) {
    return false; // backend already initialized
}
```

不允许：

```text
一次 ASU 调用
  ├── 写 standalone collector
  └── 同时写 UC::Metrics
```

否则会出现：

- 同一 endpoint 重复计数。
- 两套 collector 数值不一致。
- 两个 exporter 对 Histogram 使用不同 buckets。
- 无法判断 Grafana 查询的是哪份数据。

只有在明确做双写迁移验证时才临时启用，并给两套指标加不同前缀，不能用于正式运行。

### 16.13 No-op backend

建议提供一个无指标构建或 no-op backend：

```cpp
class NoopMetricsBackend final : public MetricsBackend {
public:
    bool Start() override { return true; }
    void Add(std::string_view, double) noexcept override {}
    void Set(std::string_view, double) noexcept override {}
    void Observe(std::string_view, double) noexcept override {}
    void Flush() override {}
    void Stop() override {}
};
```

这样 ASU 业务代码不需要到处写条件编译：

```cpp
#ifdef ENABLE_METRICS
...
#endif
```

统一调用 facade，由 backend 决定是否工作。

### 16.14 实施步骤

如果确定 standalone 包不能携带 UCM，建议按照以下顺序实施：

1. 建立 `ASU::Metrics` facade、backend 接口和 no-op backend。
2. 将新增 ASU 埋点全部改为只调用 facade。
3. 实现 standalone registry，先完成 Counter/Gauge/Histogram 单元测试。
4. 实现 dispatcher 和 Prometheus text serializer。
5. 实现最小 HTTP server，接入 `kv-test` 生命周期。
6. 使用 `curl` 和长时间 bench 验证 standalone 闭环。
7. 实现可选 `UcmMetricsBackend`，转发到 `UC::Metrics`。
8. 用同一组 descriptor/配置验证两个 backend 输出的指标契约一致。
9. 最后再导入或新增 Grafana panel。

### 16.15 兼容性验收

同一组测试输入分别运行 standalone 和 UCM backend，至少比较：

```text
metric name
metric type
HELP
unit
labels
histogram bucket boundaries
histogram count/sum
counter 增量
gauge 最终值
```

允许 `job`、`instance`、`model_name` 等部署标签的具体值不同，但 PromQL 所依赖的 label key 应保持兼容。

最终可以把两边 `/metrics` 过滤后做契约测试：

```bash
curl -s http://127.0.0.1:9108/metrics | grep '^ucm:asu_' > standalone.metrics
curl -s http://127.0.0.1:8000/metrics | grep '^ucm:asu_' > ucm.metrics
```

测试重点不是要求数值完全相同，而是要求指标集合、类型、单位和 Histogram 结构一致。

## 17. 最终建议

如果 standalone 包允许 UCM 依赖，建议采用以下落地路线：

```text
第一步：metrics STATIC -> 共享 libucm_metrics.so
第二步：ASU 埋点统一写入共享 collector
第三步：公共目录实现 MetricsDispatcher + C++ HTTP exporter
第四步：kv-test 负责 exporter 生命周期，不负责指标实现
第五步：同一份 metrics_configs.yaml 定义 name/type/help/buckets
第六步：vLLM 保留现有 Python adapter，standalone 使用 C++ adapter
第七步：Prometheus 分别抓 vLLM 或 kv-test endpoint，Grafana 复用指标名
```

核心原则是：

> ASU 只生产指标；共享 metrics core 只聚合指标；宿主决定用哪种 exporter；Prometheus 负责抓取，Grafana 负责展示。

这样既能让 `kv-test` 在没有 vLLM 时独立暴露 metrics，也不会破坏 UCM/vLLM 当前从 `ucmmetrics` 获取指标的路径。

如果 standalone 包明确不能包含 UCM，则采用：

```text
ASU::Metrics facade
    ├── StandaloneMetricsBackend -> ASU collector + /metrics
    └── UcmMetricsBackend        -> UC::Metrics
```

这时需要自己实现一套纯 C++ collector/exporter，但兼容性的核心不是复制 UCM 内部实现，而是让两种 backend 遵守同一份 metric descriptor、命名、类型、单位、buckets 和 label 契约。

## 18. 当前代码落地情况

本方案已经在当前分支实现，主要代码如下：

```text
ucm/transport/kv/asu/metrics/
├── include/asu_metrics/metrics.h
├── include/asu_metrics/metric_names.h
├── include/asu_metrics/ucm_metrics_backend.h
├── src/metrics.cpp
├── src/standalone_metrics_backend.cpp
└── src/ucm_metrics_backend.cpp
```

实现内容：

- `libasu_metrics.so` 保存唯一的 `UC::ASU::Metrics` facade/backend。
- standalone backend 自己维护 Counter/Gauge/Histogram，不依赖 UCM、Python、prometheus-cpp 或 yaml-cpp。
- 内置轻量 YAML definition loader，兼容现有 `metrics_configs.yaml` 中的 prefix、name、documentation 和 inline buckets；它不是通用 YAML 解析器，不支持 anchor、multiline 等扩展语法。
- 内置 Linux HTTP server，提供 `/metrics` 和 `/health`（与 vLLM 一致）。
- `kv-test` 从 key-value 配置读取监听地址、端口、labels 和退出 grace period。
- ASU client 已在 query/load/store/batch-load/batch-store/delete/wait 路径增加首批埋点。
- 可选 `UcmMetricsBackend` 将同一批 facade 调用转发到 `UC::Metrics`。
- UCM metrics core 已调整为共享 `libucm_metrics.so`，保证 Python `ucmmetrics` 与 adapter 使用同一个 collector。

Standalone 构建保持 adapter 关闭：

```bash
cmake -S . -B build-kv-test \
  -DBUILD_UCM_ASU=ON \
  -DBUILD_UCM_STORE=OFF \
  -DBUILD_ASU_UCM_METRICS_ADAPTER=OFF \
  -DRUNTIME_ENVIRONMENT=ascend
```

此时 `kv-test`、`libasu_client.so` 和 `libasu_transport.so` 只依赖 `libasu_metrics.so`，不依赖 `libucm_metrics.so`。

UCM/AsuStore 构建显式开启 adapter：

```bash
cmake -S . -B build-ucm-asu \
  -DBUILD_UCM_ASU=ON \
  -DBUILD_UCM_STORE=ON \
  -DBUILD_ASU_UCM_METRICS_ADAPTER=ON \
  -DRUNTIME_ENVIRONMENT=ascend
```

`AsuStore` 会安装 `UcmMetricsBackend`；最后一个 store 释放时，如果 backend 是由 AsuStore 创建的，也会安全关闭，避免动态库卸载后留下失效的 backend vtable。
