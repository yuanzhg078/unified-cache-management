# KV client I/O 性能指标 Story 设计

> 本文描述 KV client I/O 性能指标在 standalone 与 UCM 两种运行模式下的完整设计。指标清单与构建细节见 [KV Metrics Facade 双 backend 设计说明](../../../docs/source/user-guide/metrics/kv_metrics_facade_dual_backend_design_zh.md)。

## 1. Story 需求描述

本 Story 的核心需求是建立 KV client I/O 路径的性能统计能力。对于查询、加载、写入、删除等操作，系统应记录请求量、处理条目数、错误和超时次数，并统计请求从提交到完成的整体耗时及关键阶段耗时。使用者需要据此观察不同操作的执行情况，区分排队、处理、发送和等待完成等阶段的开销，定位性能瓶颈，并评估负载变化或代码调整对 I/O 性能的影响。

这套性能数据需要在独立 `kv-test` 和 UCM `AsuStore` 两种运行场景下都能被采集和查看。相同的 KV 操作在两种场景中应使用一致的指标名称、单位和统计含义，便于对比与持续监控。指标能力应由宿主按需启用；关闭时，KV 请求仍按原有语义执行，性能埋点不应影响正常 I/O 结果。

本 Story 覆盖现有 KV 埋点在两种运行场景中的采集接入、指标定义对齐、结果导出，以及 KV client Grafana 面板模板的查询适配。它不改变 KV 请求的执行语义；Prometheus 和 Grafana 服务的安装部署由使用方负责。

## 2. Story 背景描述

KV client 的 I/O 路径跨越请求提交、任务排队、处理、发送和完成等阶段。请求变慢时，仅凭最终成功状态或端到端耗时，无法判断时间消耗在客户端排队、任务处理、传输发送还是等待完成；错误和超时也需要与相应阶段的耗时、请求量结合分析。因此，KV 需要一套能够分阶段观察 I/O 路径的性能指标框架，让调用次数、处理条目数、错误与超时次数，以及各阶段耗时采用明确、稳定的统计口径。

这套性能指标框架由 I/O 埋点、统一写入接口、指标定义以及采集导出链路组成。KV client/transport 记录性能事件，metrics facade 接收埋点更新，指标定义约定 Counter 和 Histogram 的名称、类型及耗时分桶。独立 `kv-test` 运行时由 KV 自身完成聚合和 HTTP 导出；UCM 运行时由 UCM 的指标体系完成采集和导出。业务路径只负责记录性能事件，指标的聚合和对外提供由运行环境承担。

UCM 的 `AsuStore` 复用同一 KV client。要在该场景下获得同样的性能数据，需要把 KV 埋点接入 UCM 的指标采集与导出链路，并在 UCM 指标配置中注册相应点位。本 Story 在统一埋点接口的基础上完成两种运行环境的接入，使相同的 I/O 路径按一致口径输出性能指标。

本文使用以下术语：

| 术语 | 含义 |
| --- | --- |
| facade | KV 业务调用的统一 C++ 接口，负责转发更新和管理进程内唯一 backend |
| backend | facade 安装的具体写入实现；运行时选择 standalone 或 UCM adapter |
| collector | 接收并聚合指标值的组件；两种模式分别使用 KV collector 和 `UC::Metrics` collector |
| exporter | 向外提供指标的组件；两种模式分别使用 KV HTTP 服务和 UCM Python consumer |

## 3. Story 用户使用场景分析

### 3.1 Story 用户与协作者

| 角色 | 类型 | 目标 | 使用的主要能力 |
| --- | --- | --- | --- |
| `kv-test` 的 `MetricsRuntime` | standalone 生命周期用户 | 在 KV client 启动前开放指标端点，退出时完成最后聚合 | `SetUpStandaloneMetrics()`、`Flush()`、`Shutdown()` |
| `AsuStore` 生命周期协调器 | UCM 生命周期用户 | 每个 worker 进程安装一次 UCM adapter | `InstallBackend()`、`CreateUcmKvMetricsAdapter()` |
| KV client/transport | 埋点用户 | 对业务事件提交名称和值 | `UpdateStats(CachedMetric&, double)`、`UpdateStats(MetricUpdate*, size_t)` |
| UCM Python metrics 初始化与 consumer | 外部协作者 | 注册 KV 点位并消费 UCM collector 的增量 | `setup_ucm_metrics()`、`MetricsDispatcher.drain_to_consumers()` |
| Prometheus | 外部抓取者 | 抓取选定宿主的累计指标 | standalone HTTP `/metrics` 或 UCM/vLLM endpoint |

### 3.2 使用场景总览

```mermaid
flowchart LR
    KT["kv-test"] --> U1["独立启动和抓取"]
    AS["AsuStore"] --> U2["UCM 启动和抓取"]
    KV["KV client/transport"] --> U3["业务批量打点"]
    KT --> U4["关闭与最终快照"]
    AS --> U4
    CFG["部署配置"] --> U5["缺失点位或禁用指标"]
```

场景是外部使用方式；以下三个 Shard 按生命周期、写入转换、指标出口职责划分，不与场景一一对应。

### 3.3 场景分析

| 场景 | 触发者与条件 | 前置条件 | 主成功结果 | 异常或替代结果 |
| --- | --- | --- | --- | --- |
| 独立启动和抓取 | `kv-test` 启用 metrics 后执行命令 | 地址/端口、定义文件有效 | KV 更新进入 standalone collector，HTTP 返回累计指标 | 端口占用或定义无效时启动报错且不安装半初始化 backend |
| UCM 启动和抓取 | UCM worker 加载并初始化 AsuStore | UCM 已注册 KV 点位且 consumer 可运行 | KV 更新进入原生 UCM collector，经 Python exporter 输出 | adapter 安装冲突时不得覆盖已有 backend；未注册点位被忽略并可诊断 |
| 业务批量打点 | client/transport 完成一次请求或异步任务阶段 | 进程已选择 backend；句柄在调用期间有效 | Counter 增量、Histogram 样本按数组顺序处理 | 未启用时 no-op；单个空句柄跳过；metrics 故障不改变 KV 请求结果 |
| 关闭与最终快照 | 宿主结束 KV 工作 | 所有相关业务线程完成 | standalone 最后聚合后关闭；UCM adapter 不自行 drain | 仍在写入时不得销毁 raw-pointer 热路径引用的 backend |
| 缺失点位或禁用指标 | 部署选择自定义 YAML、关闭 metrics | 配置可解析 | 已注册点位被正常采集；禁用时无附加输出 | 未注册名称不更新；配置检查指出 KV 点位缺口 |

### 3.4 场景与 Shard 映射

| 场景 | backend 生命周期 | KV 指标写入路径 | 定义与导出契约 |
| --- | --- | --- | --- |
| 独立启动和抓取 | 主责 | 接收业务更新 | 提供 standalone 描述与抓取 |
| UCM 启动和抓取 | 主责 | 转发至 UCM | UCM 注册与 consumer 主责 |
| 业务批量打点 | 提供当前 backend | 主责 | 确定类型与单位 |
| 关闭与最终快照 | 主责 | 停止写入 | 完成最终可观察结果 |
| 缺失点位或禁用指标 | 禁用时保持 no-op | 未注册时跳过 | 主责 |

## 4. Story 设计描述

### 4.1 Story 定义与总体思路

本 Story 以 KV metrics facade 作为 KV client/transport I/O 性能埋点的统一入口，业务代码只向 facade 提交请求次数、错误次数和阶段耗时等数据，由运行环境选择具体的采集方式。在独立 `kv-test` 进程中，宿主安装 standalone backend，将指标写入 KV 自身的 collector，聚合后通过 HTTP 接口提供抓取；在 UCM `AsuStore` 进程中，宿主安装 UCM adapter，把同一组埋点转发给 `UC::Metrics`，再由 UCM 的 dispatcher 和 Python exporter 对外提供指标。两种模式沿用相同的 KV 指标名称、类型、单位和耗时分桶，并由各自宿主管理启用与关闭，因此 KV I/O 代码无需区分运行环境，使用者也能按一致口径分析性能。

#### 两种模式的独立设计

| 设计环节 | Standalone：`kv-test` / 纯 KV 宿主 | UCM：AsuStore 所在 worker |
| --- | --- | --- |
| 构建依赖 | `kv-test` 链接 `kv_metrics_standalone` 和共享 facade，不链接 UCM collector | AsuStore 链接 `kv_metrics_ucm_adapter`、共享 facade 和 UCM collector，不链接 standalone HTTP exporter |
| 启用条件 | `metrics.enabled=true` 时由 `kv-test` 安装 backend | 由 UCM worker 将有效的 `enable_metrics` 决策传入 AsuStore；禁用时不安装 adapter |
| backend 选择 | 宿主调用 `SetUpStandaloneMetrics()`，内部完成 `InstallBackend()` | AsuStore 创建 UCM adapter，调用 `InstallBackend()` |
| 点位注册 | backend 加载 KV YAML；未指定文件时使用生成的内嵌描述，并建立名称/标签到 slot 的映射 | UCM Python `setup_ucm_metrics()` 从实际生效配置调用原生 `CreateStats()`；adapter 不注册点位 |
| KV 写入 | facade 转发到 standalone backend；`CachedMetric` 绑定 collector slot，数组批量进入一次线程本地写入区 | facade 转发到 UCM adapter；KV 句柄绑定原生 `UC::Metrics::CachedMetric`，数组逐项写入原生 collector |
| 数据聚合 | KV aggregation thread 把线程 buffer 的增量合入进程级累计快照 | `MetricsDispatcher` 统一调用原生 `GetAllStatsAndClear()` 取得本轮增量 |
| 对外导出 | KV 自带 HTTP server 直接返回累计 `/metrics` | UCM Python logger/consumer 累积增量，由 UCM/vLLM endpoint 导出 |
| 停止 | KV 工作线程结束后 `Flush()`，按配置保留抓取窗口，再 `Shutdown()` 停聚合线程和 HTTP server | worker 级协调器等待所有 AsuStore 的 KV 工作线程退出，再关闭其拥有的 facade backend；adapter 不 drain 或停止 UCM collector |
| 标签 | standalone 支持 `constantLabels` 和已注册的 `node_id` | Python exporter 提供 `model_name/worker_id`；现有 UCM collector 不保留 KV `node_id` |

两种模式共享 KV 业务埋点 API、facade 接口和指标基础名称、类型及单位约定；collector、注册过程、exporter 与关闭责任分别属于各自宿主。后续时序与 Shard 规则均遵循此边界。

#### KV client 如何接入 UCM metrics

`kv_client` 本身不调用 `UC::Metrics`。例如 `kv_client_impl.cc` 中的 `RecordSubmit()` 已经用 `KV_METRIC("kv_client_store_requests_total")` 构造 KV 句柄，并把 `{句柄, 值}` 数组交给 `kv::metrics::UpdateStats()`。兼容由宿主安装的 UCM adapter 在调用链中完成：

```text
KvClientImpl / transport
  → kv::metrics::UpdateStats(kv::metrics::CachedMetric, value)
  → libkv_metrics.so 中当前安装的 UcmKvMetricsAdapter
  → 将 KV CachedMetric 的基础名称绑定到 UC::Metrics::CachedMetric
  → UC::Metrics::UpdateStats(nativeMetric, value)
  → libucm_metrics.so 的线程 buffer
  → MetricsDispatcher 唯一 drain → UCM Python exporter
```

这里的两个 `CachedMetric` 是不同的 C++ 类型；adapter 的 `UcmMetricBinding` 是它们之间的桥。首次写入时，以 KV 句柄的 `Name()` 构造原生句柄并由 KV 句柄持有；之后复用原生句柄，原生 `id/seenEpoch` 负责查询 UCM 注册表。adapter 传递原始 value，不再次计算 Counter、Histogram 或 duration 单位，也不在 C++ 名称前加 `ucm:` 前缀。

UCM 路径产生可导出指标需要满足三项条件：AsuStore 在 `KvClient::Init()` 前把 adapter 安装到**与 kv_client 共用的** `libkv_metrics.so`；UCM 的 `setup_ucm_metrics()` 已在**与 adapter 共用的** `libucm_metrics.so` 中注册同名且同类型的 KV 点位；Python consumer 对该 collector 执行 drain 并导出。指标关闭时不安装 adapter，facade 没有 backend，KV 指标更新为 no-op。

### 4.2 逻辑模型

```mermaid
flowchart TB
    subgraph SYS["Unified Cache Management System"]
        subgraph KV["KV Semantics SubDomain"]
            KTEST["Component: kv-test"]
            INSTR["Component: KV client/transport 埋点"]
            subgraph MET["Component: KV Metrics"]
                F["Module: facade"]
                subgraph STPATH["Standalone 模式"]
                    ST["Module: standalone backend"]
                    STCOL["Module: 线程 buffer 与累计 collector"]
                    STHTTP["Module: HTTP exporter"]
                end
                subgraph UCPATH["UCM 模式"]
                    UA["Module: UCM adapter"]
                end
            end
        end
        subgraph UC["UCM SubDomain"]
            ASU["Component: AsuStore"]
            COL["Component: UC::Metrics collector"]
            DISP["Component: MetricsDispatcher"]
            EXP["Component: Python metrics exporter"]
        end
    end
    PROM["External: Prometheus"]
    INSTR -. "Usage: UpdateStats" .-> F
    KTEST -. "Usage: SetUpStandaloneMetrics" .-> ST
    ASU -. "Usage: InstallBackend" .-> F
    ASU -. "Usage: adapter factory" .-> UA
    F -. "Usage: standalone 运行时" .-> ST
    F -. "Usage: UCM 运行时" .-> UA
    ST -. "Usage: 采集" .-> STCOL
    ST -. "Usage: HTTP 服务" .-> STHTTP
    STHTTP -. "Usage: 读取累计快照" .-> STCOL
    UA -. "Usage: 写入" .-> COL
    EXP -. "Usage: 取本轮指标" .-> DISP
    DISP -. "Usage: drain" .-> COL
    PROM -. "scrape" .-> STHTTP
    PROM -. "scrape" .-> EXP
```

逻辑模型表明，KV client/transport 只面向统一的指标入口，不依赖具体的采集和导出组件。独立运行场景使用 KV 自身的采集与 HTTP 导出能力；UCM 场景则复用原生 collector、dispatcher 和 Python 导出能力。两条路径在各自宿主内完成指标的采集与对外提供，业务埋点保持一致。

### 4.3 实现结构模型

`KvMetricsBackend` 与 `kv::metrics::CachedMetric` 是两种模式共用的类型。下面分别画出它们在 standalone 和 UCM 模式下关联的具体类；同一进程只会选择其中一张图对应的 backend。

**Standalone 类图：**

```mermaid
classDiagram
    direction TB
    class KvMetricsBackend {
        <<interface>>
        +UpdateStats(metric: CachedMetric&, value: double) void
        +UpdateStats(updates: const MetricUpdate*, count: size_t) void
        +RegisterMetricLabels(name: string, labels: MetricLabels) bool
        +Flush() void
        +Stop() void
    }
    class StandaloneKvMetricsBackend {
        -StandaloneMetricsConfig config_
        -unique_ptr collector_
        -unique_ptr server_
        -string error_
        +Initialize(descriptors: MetricDescriptorList) bool
        +UpdateStats(metric: CachedMetric&, value: double) void
        +UpdateStats(updates: const MetricUpdate*, count: size_t) void
        +RegisterMetricLabels(name: string, labels: MetricLabels) bool
        +Flush() void
        +Stop() void
        +LastError() const string&
    }
    class ThreadBufferedMetricsCollector {
        -descriptors_
        -generation_
        -metricIds_
        -buffers_
        -snapshot_
        -aggregator_
        +Register(descriptor: MetricDescriptor, error: string&) bool
        +RegisterMetricLabels(name: string, labels: MetricLabels) bool
        +StartAggregation(intervalMs: uint32_t) void
        +StopAggregation() void
        +UpdateStats(updates: const MetricUpdate*, count: size_t) void
        +Flush() void
        +Render(prefix: string, labels: MetricLabels) string
    }
    class ThreadBuffer {
        -slots[2]
        -writeIndex
        -activeWriteIndex
        -retired
        +SwitchWriteSlot() int
    }
    class MetricsHttpServer {
        -registry_
        -StandaloneMetricsConfig config_
        -running_
        -listenFd_
        -worker_
        +Start(error: string&) bool
        +Stop() void
    }
    class CachedMetric {
        -string name_
        -MetricLabels labels_
        -mutex mutex_
        -unique_ptr owner_
        -atomic binding_
        +Name() const string&
        +Labels() const MetricLabels&
        +Resolve(factory: Factory) Binding*
    }
    class Binding {
        <<interface>>
    }
    class SlotBinding {
        -slot
    }
    StandaloneKvMetricsBackend ..|> KvMetricsBackend
    StandaloneKvMetricsBackend *-- ThreadBufferedMetricsCollector
    StandaloneKvMetricsBackend *-- MetricsHttpServer
    ThreadBufferedMetricsCollector o-- ThreadBuffer : retains shared buffers
    MetricsHttpServer --> ThreadBufferedMetricsCollector : registry reference
    ThreadBufferedMetricsCollector ..> CachedMetric : resolves
    CachedMetric *-- Binding : owns one
    SlotBinding ..|> Binding
```

图中只列影响指标写入、聚合和导出的关键成员与接口。`MetricDescriptorList` 是排版简写，对应 `const std::vector<MetricDescriptor>&`；方法签名省略了成员函数自身的 `const` 和 `noexcept` 修饰。Standalone backend 独占 collector 和 HTTP server；collector 持有各业务线程共享的 `ThreadBuffer`，将增量聚合到 `snapshot_`。collector 在首次遇到 KV 句柄时创建 `SlotBinding`，由 `CachedMetric` 持有并缓存对应的 slot；HTTP server 通过 `registry_` 引用 collector，读取累计快照。图中的 `Binding` 对应代码中的 `CachedMetric::Binding`。

**UCM 类图：**

```mermaid
classDiagram
    direction TB
    class KvMetricsBackend {
        <<interface>>
        +UpdateStats(metric: CachedMetric&, value: double) void
        +UpdateStats(updates: const MetricUpdate*, count: size_t) void
        +RegisterMetricLabels(name: string, labels: MetricLabels) bool
        +Flush() void
        +Stop() void
    }
    class UcmKvMetricsAdapter {
        +UpdateStats(metric: CachedMetric&, value: double) void
        +UpdateStats(updates: const MetricUpdate*, count: size_t) void
        +RegisterMetricLabels(name: string, labels: MetricLabels) bool
        +Flush() void
        +Stop() void
    }
    class CachedMetric {
        -string name_
        -MetricLabels labels_
        -mutex mutex_
        -unique_ptr owner_
        -atomic binding_
        +Name() const string&
        +Labels() const MetricLabels&
        +Resolve(factory: Factory) Binding*
    }
    class Binding {
        <<interface>>
    }
    class UcmMetricBinding {
        -UcCachedMetric nativeMetric_
    }
    class UcCachedMetric {
        +string name
        +atomic id
        +atomic seenEpoch
    }
    class UcMetricsCollector {
        -nameToId_
        -registerEpoch_
        +CreateStats(name: string, type: string, buckets: HistogramBuckets) void
        +UpdateStats(nativeMetric: UcCachedMetric&, value: double) void
        +GetAllStatsAndClear() StatsSnapshot
    }
    UcmKvMetricsAdapter ..|> KvMetricsBackend
    UcmKvMetricsAdapter ..> CachedMetric : resolves
    CachedMetric *-- Binding : owns one
    UcmMetricBinding ..|> Binding
    UcmMetricBinding *-- UcCachedMetric
    UcmKvMetricsAdapter ..> UcMetricsCollector : writes updates
    UcMetricsCollector ..> UcCachedMetric : resolves metric id
```

`HistogramBuckets` 是 `const std::vector<double>&` 的排版简写；`StatsSnapshot` 表示 `UC::Metrics::Metrics::GetAllStatsAndClear()` 返回的 Counter、Gauge 与 Histogram 三元组。图中的方法签名省略了成员函数自身的 `const` 和 `noexcept` 修饰。KV `CachedMetric` 保存指标名称、标签和首次解析后缓存的 binding；其中 `owner_` 持有 binding，`binding_` 提供后续更新的快速访问。UCM adapter 本身不保存每个指标的状态，首次写入时创建 `UcmMetricBinding`，由它持有原生 `UC::Metrics::CachedMetric`（图中的 `UcCachedMetric`）。原生句柄保存名称、指标 ID 和注册 epoch；adapter 调用 `UC::Metrics::Metrics`（图中的 `UcMetricsCollector`）写入指标，不拥有该 collector。collector 与 Python exporter 的关系在 4.2 节逻辑模型中展示。

两张图的 `CachedMetric` 都指 `kv::metrics::CachedMetric`，但它在一个进程中只拥有一种具体 binding。facade 的全局 `shared_ptr<KvMetricsBackend>` 持有所选 backend，`gBackendFast` 是无所有权的热路径指针；宿主如何安装 backend 见 4.4 节上下文模型。当前设计不支持先用 standalone 解析句柄、再切换成 UCM adapter 复用该句柄。

### 4.4 Story 上下文模型

```mermaid
flowchart LR
    KT["kv-test"]
    AS["AsuStore"]
    KV["KV client/transport"]
    subgraph STORY["KV Metrics Story 边界"]
        SETUP["Provided: SetUpStandaloneMetrics(config)"]
        FACTORY["Provided: CreateUcmKvMetricsAdapter()"]
        INSTALL["Provided: InstallBackend(backend, error)"]
        UPDATE["Provided: UpdateStats(metric/value 或 updates/count)"]
        F["facade"]
        ST["standalone backend"]
        UA["UCM adapter"]
        HTTP["Provided: KV HTTP /metrics"]
    end
    UCAPI["Required: UC::Metrics::UpdateStats(nativeMetric, value)"]
    PROM["Prometheus"]
    KT -. "Usage" .-> SETUP
    SETUP --> ST
    AS -. "Usage" .-> FACTORY
    FACTORY --> UA
    AS -. "Usage" .-> INSTALL
    INSTALL --> F
    KV -. "Usage" .-> UPDATE
    UPDATE --> F
    F -. "运行时选择" .-> ST
    F -. "运行时选择" .-> UA
    ST --> HTTP
    UA -. "Usage" .-> UCAPI
    PROM -. "scrape" .-> HTTP
```

图中列出本 Story 对直接调用者提供的接口，以及 UCM adapter 所依赖的原生写入接口。`SetUpStandaloneMetrics()` 在 standalone backend 内启动 KV HTTP exporter；`setup_ucm_metrics()` 和 Python dispatcher 属于 UCM 侧的注册与导出链路，已在 4.2 节逻辑模型及 4.5.4 节时序中展示。两条 facade 连线仍表示互斥的运行时选择。

### 4.5 Story 运行时序

#### 4.5.1 总体运行时序

```mermaid
sequenceDiagram
    autonumber
    participant H as 宿主
    participant R as 指标注册/配置
    participant F as KV facade
    participant B as 所选 backend
    participant K as KV client/transport
    participant C as 所选 collector
    participant E as 所选 exporter
    participant P as Prometheus
    H->>R: 装载所选模式的指标定义
    R-->>H: 定义可用
    H->>B: 创建/初始化 backend
    H->>F: InstallBackend(backend)
    alt 安装成功
        H->>K: 启动 KV 工作
        K->>F: UpdateStats(metric/value)
        F->>B: UpdateStats(metric/value)
        B->>C: 写入指标值
        E->>C: 读取累计快照或消费增量
        C-->>E: 当前可导出数据
        P->>E: 抓取 /metrics
        E-->>P: 指标结果
        H->>K: 停止并等待所有 KV 工作线程
        H->>F: Flush / 有所有权时 Shutdown
    else 冲突或初始化失败
        F-->>H: false + error
        H->>B: 清理本次创建的资源
    end
```

时序步骤：

1. 宿主选择模式，并准备该模式的注册描述；standalone 在 C++ backend 内完成描述加载，UCM 由 Python `setup_ucm_metrics` 注册。
2. 宿主安装唯一 backend，失败时只清理自己创建的对象，不关闭进程中已有 backend。
3. KV 业务只调用 facade，写入被选中的 collector；exporter 读取该 collector 的结果。
4. 退出时先停 KV 工作线程。standalone 最后 `Flush/Shutdown`；UCM adapter 不 drain，且关闭必须由拥有进程级生命周期的宿主协调。

#### 4.5.2 backend 安装与停止时序

```mermaid
sequenceDiagram
    autonumber
    participant H as 宿主协调器
    participant F as facade
    participant B as backend
    participant K as KV 工作线程
    H->>B: 创建并完成本模式初始化
    H->>F: InstallBackend(shared_ptr<B>, error)
    alt 尚无 backend
        F-->>H: true
        H->>K: 启动业务
        H->>K: 停写并 join
        H->>F: Flush()
        opt 拥有进程级关闭权且不会再安装
            H->>F: Shutdown()
        end
    else 已有 backend
        F-->>H: false + already initialized
        H->>B: 释放本次创建的 backend
    end
```

时序步骤：

1. standalone 初始化包含 HTTP/聚合线程；失败不安装。UCM adapter 创建不启动 exporter。
2. `InstallBackend` 在 facade 锁内拒绝第二个 backend；AsuStore 的多个实例经协调器共享第一次安装的 backend。
3. 所有可能调用 facade 的 KV 工作线程先停，再允许关闭。当前 `gBackendFast` 不持有引用，`Shutdown` 不能与业务更新并发。

#### 4.5.3 Standalone 注册、写入与抓取时序

```mermaid
sequenceDiagram
    autonumber
    participant T as kv-test MetricsRuntime
    participant B as Standalone backend
    participant F as KV facade
    participant K as KV client/transport
    participant P as Prometheus
    T->>B: SetUpStandaloneMetrics(config)
    B->>B: 加载 YAML 或默认描述，注册 slot
    B->>B: 启动聚合线程和 HTTP server
    B->>F: InstallBackend(backend)
    F-->>B: 安装成功
    B-->>T: SetUpStandaloneMetrics 成功
    T->>K: 初始化并执行业务
    K->>F: UpdateStats(KV CachedMetric, value)
    F->>B: UpdateStats(KV CachedMetric, value)
    B->>B: 更新线程本地双 buffer
    loop aggregationIntervalMs
        B->>B: 切槽并合并累计快照
    end
    P->>B: GET /metrics
    B-->>P: 当前累计快照
    T->>K: Shutdown 并等待业务线程
    T->>F: Flush()，等待抓取窗口，Shutdown()
```

时序步骤：

1. `kv-test` 在 KV client 初始化前启动 standalone backend；它自己注册指标描述、聚合线程和 HTTP server，随后把 backend 安装到 facade。
2. KV 业务仍只调用 facade。standalone backend 将 KV 句柄解析为本 collector 的 slot，把 Counter/Histogram 更新写入线程本地 buffer；聚合线程生成累计快照。
3. HTTP 抓取读取累计快照，不调用 UCM collector，也不清空已导出的 Counter/Histogram。关闭前先停业务线程，再 `Flush()` 并按 `shutdownGraceMs` 保留最后抓取窗口。

#### 4.5.4 UCM 注册、安装与导出主时序

下图以启用 `multiproc` consumer 的 `PrometheusStatsLogger` 路径为例。vLLM connector consumer 使用同一 `MetricsDispatcher` 的独立消费缓冲，接入位置见本节后的说明。

```mermaid
sequenceDiagram
    autonumber
    participant P as UCM Python 初始化
    participant S as AsuStore
    participant K as KV client
    participant F as KV facade
    participant A as UCM adapter
    participant C as UC::Metrics collector
    participant D as MetricsDispatcher
    participant L as PrometheusStatsLogger
    P->>L: 创建 PrometheusStatsLogger(config)
    L->>C: setup_ucm_metrics: SetUp + CreateStats(KV 定义)
    L->>D: get_metrics_dispatcher(config)
    S->>A: CreateUcmKvMetricsAdapter()
    S->>F: InstallBackend(adapter)
    alt 安装成功
        F-->>S: true
        S->>K: KvClient::Init()
        K->>F: UpdateStats(KV CachedMetric, value)
        F->>A: UpdateStats(KV CachedMetric, value)
        A->>C: UpdateStats(UC CachedMetric, value)
        loop log_interval
            L->>D: drain_to_consumers()
            D->>C: GetAllStatsAndClear()
            C-->>D: counter/gauge/histogram 增量
            L->>D: get_stats_and_clear(multiproc)
            D-->>L: 本轮增量
            L->>L: 累积 Prometheus 对象
        end
    else 已有其他 KV backend
        F-->>S: false + error
        S->>A: 释放本次创建的 adapter
    end
```

时序步骤：

1. UCM 初始化读取实际生效的 YAML 或默认配置，按基础名称、类型和 bucket 向 `UC::Metrics` **注册 KV 点位**，并准备 Python consumer。
2. AsuStore 在 `KvClient::Init()` 前创建 UCM adapter，调用 `kv::metrics::InstallBackend(adapter)` **向 KV facade 安装 backend**。这一步决定 `kv_client` 后续的 facade 调用是否能转到 UCM；`InstallBackend` 不负责注册指标定义。安装失败时只释放本次 adapter，不覆盖已有 backend。
3. `kv_client` 继续向 facade 写 `kv::metrics::CachedMetric`；facade 调用 adapter，adapter 按基础名称绑定原生 `UC::Metrics::CachedMetric`，把值写入与 Python consumer 共用的 `libucm_metrics.so`。adapter 不调用 `CreateStats` 或 `GetAllStatsAndClear`。
4. 在图示的 multiproc 路径中，logger 通过 dispatcher 统一 drain collector，并将本轮增量累积到 Prometheus 对象。vLLM connector 路径由 connector 调用同一个 dispatcher 的 `drain_to_consumers()`，再通过 `get_stats_and_clear(VLLM_CONNECTOR_CONSUMER)` 取得自身缓冲；两个 consumer 不分别直接 drain 原生 collector。应在各自最终对外出口上比较累计指标，而不是拿 UCM C++ 的单轮增量直接与 standalone 快照比较。

#### 4.5.5 UCM 单点与批量写入细节时序

```mermaid
sequenceDiagram
    autonumber
    participant K as KV 埋点
    participant F as facade
    participant A as UCM adapter
    participant M as KV CachedMetric
    participant C as UC::Metrics
    K->>F: UpdateStats(updates, count)
    F->>A: UpdateStats(updates, count)
    loop 数组顺序遍历
        A->>M: Resolve(native CachedMetric binding)
        M-->>A: nativeMetric
        A->>C: UpdateStats(nativeMetric, value)
    end
    C-->>A: 已写入或未注册时跳过
```

时序步骤：

1. facade 对空数组或零长度直接返回；adapter 对数组里的空 `metric` 跳过。
2. KV `CachedMetric::Resolve` 首次创建 UCM binding，后续复用；原生 `CachedMetric` 自己维护注册 ID 和 epoch，晚注册时可重新解析。
3. adapter 保留数组顺序与重复名称，每个 Histogram 条目仍是一个样本；不得先转为 `unordered_map`。原生接口可能抛出异常，adapter 的 `noexcept` 边界捕获并记录低频诊断，不影响 KV 请求。

## 5. backend 选择与生命周期

### 5.1 设计描述

**Standalone**：`MetricsRuntime::Start` 仅在 `metrics.enabled=true` 时调用 `SetUpStandaloneMetrics`，并在 KV client 初始化前完成描述加载、聚合线程、HTTP server 启动及 facade 安装。`clientRunner.Shutdown()` 等待 KV 工作结束后，`MetricsRuntime::Stop` 执行最后一次 `Flush()`，按配置保留抓取窗口，随后调用 `Shutdown()`。

**UCM**：UCM worker 将有效的 `enable_metrics` 决策传给 AsuStore。启用时，由 worker 级协调器在首个 AsuStore 的 `KvClient::Init()` 前安装一次 UCM adapter；禁用时不安装。多个 AsuStore 实例共享该 backend，单个实例 Setup 失败或析构只释放其使用引用，不关闭全局 backend。worker 停止全部 KV 工作线程后，由该协调器调用一次 facade `Shutdown()`；它不得调用 UCM collector 的 drain 或关闭接口。

worker 级一次安装也是 `CachedMetric` 当前没有 binding generation 的要求：AsuStore 在同一 worker 中反复创建时，不执行“关闭后重装”。若宿主允许卸载 AsuStore 动态库，必须先完成上述停写和 facade 关闭，再执行 `dlclose`，避免 backend 虚函数指向已卸载代码。

### 5.2 重点实现接口

`bool InstallBackend(std::shared_ptr<KvMetricsBackend> backend, std::string* error = nullptr)`：入参为非空 backend 与可选错误输出；成功发布唯一 backend，已有 backend 或空指针时返回 `false` 和错误，不替换旧对象。该接口为现有 facade API。

`bool SetUpStandaloneMetrics(StandaloneMetricsConfig config, std::string* error = nullptr)`：加载定义并启动 standalone collector/exporter，然后安装到 facade；失败返回 `false` 并清理本次已启动资源。该接口已存在。

`std::shared_ptr<KvMetricsBackend> CreateUcmKvMetricsAdapter()`：创建只写入 `UC::Metrics` 的 backend；不启动 Python consumer，也不注册点位。调用方持有返回对象直到交给 `InstallBackend`。

`void Flush()` / `void Shutdown()`：现有 facade API；standalone 的 `Flush` 聚合增量，`Shutdown` 清理 server/collector。UCM adapter 的对应操作为空；`Shutdown` 只能由确认 KV 停写且拥有 backend 生命周期的宿主调用。

### 5.3 重点依赖接口

| 依赖接口 | 用途 | 失败处理 |
| --- | --- | --- |
| `StandaloneKvMetricsBackend::Initialize(...)` | 启动 standalone 采集和 HTTP | 失败时停止已启动资源，不调用 `InstallBackend` |
| `KvClient::Init()` / `Shutdown()` | 使 metrics 生命周期覆盖 KV 工作 | 初始化失败释放本实例引用；关闭时先等待业务线程退出 |
| `UC::Metrics` 共享库加载 | 保证 adapter 与 Python consumer 共用 collector | 安装包检查 RPATH/SONAME；不满足时视为部署失败 |

### 5.4 关键约束与验收

| 约束 | 验收方式与预期结果 |
| --- | --- |
| 全进程同时只有一个 KV backend | 连续安装两次，第二次返回 `false`，第一次仍可接收更新 |
| 未启用指标时保持 no-op | 不安装 backend；`IsEnabled()==false`，业务调用不产生 collector/HTTP 副作用 |
| 停写先于销毁 | 并发业务线程全部 join 后再 `Shutdown`；无悬挂访问或丢失最后已完成更新 |
| 当前版本不重装/切换 backend | 测试或宿主不在同一进程复用已绑定句柄跨模式；若将来需要，先设计 binding generation |
| AsuStore 多实例共用一次安装 | 并发 Setup 后 facade 仍只有一个 adapter，单实例销毁不影响其余实例 |

## 6. KV 指标写入路径

### 6.1 设计描述

KV client/transport 在两种模式中都使用已有 `KV_METRIC`、`CachedMetric` 和 `MetricUpdate`，但句柄的 binding 与最终 collector 不同：

- **Standalone**：`CachedMetric::Resolve()` 首次查找由 KV YAML/default descriptor 注册的名称与标签组合，缓存 `SlotBinding`。业务线程进入本线程的双 buffer，数组批量共用一次 `WriteGuard`；聚合线程负责把增量合入累计快照。
- **UCM**：adapter 在 KV 句柄中缓存 `UcmMetricBinding`，其中持有原生 `UC::Metrics::CachedMetric`。数组逐项调用原生单点更新；原生句柄的 `registerEpoch_` 可在点位晚注册后重试解析。adapter 不缓存数值，不建立第二套 exporter。

`TransportTaskExecutor` 的两项 node 级 Histogram 使用 `MetricLabels{{"node_id", ...}}`。现有 UCM collector 只有按基础名称的键，不支持把标签穿透到 Python；首版 UCM adapter 把它们聚合到基础名称下，`RegisterMetricLabels` 返回 `false`，明确不承诺 UCM 保留 `node_id`。若 node 维度是上线要求，需作为另一个扩展设计修改 UCM collector、drain 数据结构和 Python exporter，不在此 Story 的首版验收内。

### 6.2 重点实现接口

`void UpdateStats(CachedMetric& metric, double value) noexcept`：现有 facade 入口。KV 句柄和值为入参，无返回值；无 backend 时直接返回，backend 异常不能越过 `noexcept` 边界。

`void UpdateStats(const MetricUpdate* updates, std::size_t count) noexcept`：现有批量入口。数组可含同名多条更新，保持顺序；空数组、零长度或数组内空指针不产生更新。UCM adapter 的 override 实现逐条转发。

`bool RegisterMetricLabels(const std::string& name, const MetricLabels& labels) noexcept`：现有 facade 入口。standalone 用于注册名称/标签组合；UCM adapter 返回 `false`，表示当前 UCM 路径不支持该标签注册。

### 6.3 重点依赖接口

| 依赖接口 | 用途 | 失败处理 |
| --- | --- | --- |
| `CachedMetric::Resolve(factory)` | 首次发布 backend 专属 binding | 创建失败由 adapter 捕获，跳过本次 metrics 更新 |
| `UC::Metrics::UpdateStats(UC::Metrics::CachedMetric&, double)` | 写入 UCM 原生线程 buffer | 未初始化/未注册时由原生实现跳过；异常在 adapter 捕获 |
| `ThreadBufferedMetricsCollector::UpdateStats(...)` | standalone 批量更新 | backend 内部处理采集异常，业务请求不受影响 |

### 6.4 关键约束与验收

| 约束 | 验收方式与预期结果 |
| --- | --- |
| 同名重复批量更新不合并 | 分别在两种 backend 下对同一 Counter 传两条更新，结果均为两值之和；Histogram `_count` 均增加 2 |
| late registration 可生效 | 先对未注册原生名称打点，再 `CreateStats`，后续调用可以出现在 UCM drain 中 |
| metrics 故障不改变 KV 结果 | 注入 binding 创建或原生更新异常，KV 请求状态保持原值且进程不终止 |
| node 标签降级明确 | UCM 两个 node Histogram 按基础名汇总，`RegisterMetricLabels` 返回 `false`；standalone 仍按 `node_id` 输出 |

## 7. 指标定义与出口兼容

### 7.1 设计描述

`kv_semantics/metrics/config/kv_metrics.yaml` 是 KV 点位基础名、类型和 Histogram bucket 的规范清单；`generate_kv_metrics.py` 从该文件生成 standalone 内嵌默认描述。UCM 启动时的 `setup_ucm_metrics` 只注册实际配置提供的点位。KV 定义同时维护在 `ucm/default_metrics_config.py` 与部署模板中，CI 检查基础名称、类型和 bucket 是否一致；构建过程不改写用户配置。用户自定义 YAML 若覆盖默认配置，也必须包含所需 KV 点位，否则原生 collector 会忽略更新。

两种出口的默认前缀不同：standalone 默认 `kv:`，UCM multiproc exporter 默认 `ucm:`。公共查询使用 `ucm:` 前缀，`kv-test` 通过配置采用该前缀，并提供稳定的 `model_name/worker_id` 标签；UCM Python logger 也提供这两个标签。`source` 不属于两端的公共契约。Counter/Histogram 出口比较以最终累计 Prometheus 值为准：standalone `/metrics` 是累计快照，UCM C++ drain 返回增量。

仓库提供 `examples/metrics/grafana_kv_client.json` 作为 KV client 性能面板模板，覆盖吞吐、阶段耗时、错误与超时等视图。模板查询需要与两种出口的公共指标名称及前缀保持一致；依赖 `node_id` 的节点维度面板仅适用于保留该标签的 standalone 路径，UCM 路径以不含 `node_id` 的汇总指标展示。Grafana 模板的维护和查询验证属于本 Story 的交付范围。

### 7.2 重点实现接口

`DefaultKvMetricDescriptors()`：由生成头提供 standalone 默认描述；KV YAML 是生成输入。`generate_kv_metrics.py --check` 比较 UCM 默认配置和部署模板中的基础名称、类型、Histogram buckets 与 HELP 语义；前缀不写入 C++ 句柄名。

`setup_ucm_metrics(config: dict) -> list[MetricDefinition]`：UCM 现有 Python 接口，调用 `ucmmetrics.set_up/create_stats` 注册实际生效的配置；空定义返回空列表。adapter 不调用它。

`MetricsDispatcher.drain_to_consumers() -> None`：UCM 现有 Python 接口，唯一读取 `GetAllStatsAndClear` 并分发到启用的 consumer；adapter 不调用它。

### 7.3 重点依赖接口

| 依赖接口 | 用途 | 失败处理 |
| --- | --- | --- |
| `generate_kv_metrics.py --check` | 校验 standalone 默认描述，以及 UCM 默认配置/模板的一致性 | 构建/CI 报错，停止交付漂移定义 |
| `UC::Metrics::CreateStats(name, type, buckets)` | 注册原生 UCM 点位 | 配置缺失时该名称不出现，部署检查报告缺口 |
| `PrometheusStatsLogger.update_stats_loop()` | 将 UCM delta 累积到 Prometheus 对象 | consumer 未运行时检查启动配置与 worker 进程位置 |

### 7.4 关键约束与验收

| 约束 | 验收方式与预期结果 |
| --- | --- |
| 两端同名指标类型/单位/bucket 一致 | 自动配置比对；在验证用配置中改变一个 bucket 或类型，CI 必须报错 |
| 前缀只在 exporter 增加 | 两端抓取均出现 `ucm:kv_...`，C++ 句柄仍使用 `kv_...` |
| UCM 只有统一 dispatcher drain | 同一 worker 不启动第二个 `GetAllStatsAndClear` 消费者；多 consumer 均可收到同一轮增量 |
| Histogram bucket 与 Python 对象匹配 | 注入一条样本，最终 `_bucket/_sum/_count` 一致，无 bucket mismatch 日志 |
| KV client Grafana 模板兼容两种出口 | 导入 `grafana_kv_client.json`，分别连接 standalone 和 UCM 指标源；公共吞吐、耗时、错误面板均可查询，节点维度面板只在 standalone 场景显示数据 |

## 8. Shard 协作关系与 Story 级约束

```mermaid
flowchart LR
    S1["backend 生命周期"] -->|"安装唯一 backend"| S2["KV 指标写入路径"]
    S3["指标定义与出口"] -->|"提供注册表和类型语义"| S2
    S2 -->|"写入 collector"| S3
    S1 -->|"停写后最终聚合/关闭"| S3
```

1. 安装先于 KV client/transport 开始打点；退出先停写，再关闭 backend。`gBackendFast` 是 raw pointer，不能用 `Shutdown` 与任意业务写入并发。
2. KV client、transport 与宿主必须解析到同一份 `libkv_metrics.so`；UCM adapter 与 Python `ucmmetrics` 必须解析到同一份 `libucm_metrics.so`。用运行进程的加载映射验证，不只比对文件名。
3. UCM adapter 只写原生 collector。`GetAllStatsAndClear()` 是破坏性 drain，归统一 dispatcher 使用。
4. 首版不支持 facade backend 重装或切换。AsuStore 的安装协调要覆盖 worker 内的反复实例创建，进程退出或动态库卸载前再安全关闭。
5. `MetricUpdate` 中的 `CachedMetric*` 至少存活到本次调用结束；异步任务中保存的节点级句柄不得早于该任务完成而析构。

## 9. SFMEA 分析

| 故障模式 | 影响 | 设计措施 | 可执行注入方法 | 需验证的结果 |
| --- | --- | --- | --- | --- |
| standalone 定义无效或 HTTP 端口占用 | 指标端点无法启动 | 初始化失败不安装 facade；停止已创建资源 | 用坏 YAML 或预占端口启动 `kv-test` | 返回明确错误，无残留聚合/HTTP 线程，facade 未启用 |
| 第二宿主重复安装 backend | 数据写到错误 collector 或覆盖对象 | `InstallBackend` 拒绝第二次安装 | 两线程并发安装不同 backend | 恰有一次成功；已安装 backend 仍可收数 |
| KV 写入线程与 `Shutdown` 并发 | raw pointer 悬挂访问 | 宿主必须先停写并 join | 测试栅栏令 writer 停在调用边界，同时触发宿主退出 | 宿主遵守停写屏障，退出无非法访问；不声称 facade 可任意并发关闭 |
| UCM KV 点位未注册 | 原生更新被丢弃 | 配置一致性检查、部署诊断 | 从 UCM YAML 移除一个 KV 名称 | 对应点位缺失被检查发现，其他点位继续正常 |
| 两份 `libucm_metrics.so` 被加载 | C++ 写入与 Python drain 分离 | 安装 RPATH/SONAME 验证 | 构造错误库搜索路径的集成环境 | `/proc/<pid>/maps` 显示重复实例并判定部署失败 |
| Histogram bucket 不一致 | Python 跳过一轮更新 | 跨端 bucket 校验 | 在 UCM 配置中改变一个 bucket | CI 报错；运行时不允许静默通过 |
| 另一个消费者直接调用 `GetAllStatsAndClear` | dispatcher 丢本轮增量 | 统一由 dispatcher drain | 在集成测试中人为加入第二次 drain | 能复现丢数并由启动/代码检查拒绝该拓扑 |
| adapter 更新抛异常 | metrics 干扰 KV 请求或触发 terminate | adapter `noexcept` 边界捕获 | 注入 binding 分配/原生更新异常 | KV 请求结果不变，进程存活，低频诊断可见 |
| AsuStore 多实例中一个失败或销毁 | 其他实例过早失去指标 | worker 级安装协调，不由单实例关闭全局 backend | 并发创建两实例，使其中一例 Setup 失败 | 成功实例持续打点，最终关闭仅一次 |

## 10. 开发自验证用例

### 10.1 Standalone 启动、抓取与退出

测试点：backend 安装、累计输出和最终聚合。

测试手段：启用 `kv-test` metrics 并用 fake provider 执行固定次数的 store；在命令运行期间重复抓取 `/metrics`，完成后在 grace 窗口抓取一次。

预期行为：请求 Counter 单调不减，Histogram `_count` 等于实际样本数；多次抓取不清零；退出前最后一次 Flush 的结果可见。端口占用时启动返回失败且无后台线程残留。

### 10.2 Facade 单例与无 backend 路径

测试点：唯一 backend、禁用 no-op。

测试手段：先在没有安装 backend 时调用单点和批量接口，再安装一个测试 backend，并尝试安装第二个。

预期行为：未安装时 `IsEnabled()==false` 且调用无副作用；首次安装成功，第二次返回 `false` 并保留原 backend 的计数。

### 10.3 UCM adapter 单点和重复批量

测试点：名称绑定、Counter 累加和 Histogram 样本数。

测试手段：在单进程中先 `UC::Metrics::SetUp/CreateStats`，安装 adapter，传入单点 Counter 和包含两条同名 Histogram 的数组，再通过唯一 drain 读取。

预期行为：Counter 值准确，Histogram 两个样本分别进入 bucket、`sum` 和 `count`；数组内空句柄跳过，无异常传出。

### 10.4 UCM 晚注册与缺失定义

测试点：原生 cached handle 的 epoch 行为和配置诊断。

测试手段：先调用未注册名称，再 `CreateStats` 注册并再次打点；另用缺少一个 KV 名称的配置运行检查。

预期行为：注册前更新不出现，注册后的更新可被 drain；配置检查明确指出缺失名称。

### 10.5 AsuStore 多实例与库唯一性

测试点：worker 级安装协调及同一 collector 实例。

测试手段：并发创建两个 AsuStore，其中一例在 KV client 初始化后续阶段失败；成功实例继续产生 KV 事件；读取 `/proc/<pid>/maps` 并由 Python dispatcher drain。

预期行为：facade 只安装一次，成功实例持续出数，`libkv_metrics.so` 和 `libucm_metrics.so` 各仅一份加载实例；失败实例的释放不关闭全局 backend。

### 10.6 两模式指标口径对照

测试点：基础名称、类型、单位、bucket、公共查询及 Grafana 模板一致性。

测试手段：对 standalone 与 UCM 路径注入相同的逻辑事件，抓取两个最终 Prometheus endpoint；运行配置一致性脚本和 `grafana_kv_client.json` 使用的公共 PromQL 查询。

预期行为：非节点标签的 KV 指标按相同事件数输出，duration 均为 seconds，Histogram bucket 与 `_count/_sum` 一致；UCM 的两个 node Histogram 按基础名汇总且不出现 `node_id`，此差异明确列在设计契约中。

## 11. 一句话总结

> 本 Story 使 KV client I/O 路径的性能数据在独立运行和 UCM 集成场景下都可采集、可查看，并保持一致的指标口径。
