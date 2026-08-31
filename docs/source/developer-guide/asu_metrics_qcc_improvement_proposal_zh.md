# QCC 改进意见：ASU Metrics 工具可观测性设计改进

## 一、课题信息

| 项目 | 内容 |
| --- | --- |
| 改进主题 | ASU Metrics 工具可观测性设计改进 |
| 改进类型 | 设计改进 / 质量改进 |
| 改进对象 | ASU Client、Transport、`kv-test` standalone 工具及 UCM `AsuStore` 集成路径 |
| 预期使用者 | ASU 开发、测试、交付运维及性能调优人员 |
| 课题目标 | 建立可独立运行、可接入 UCM、指标口径统一且可追溯的 ASU Metrics 能力 |

## 二、选题背景

ASU 的请求处理是异步、多层拆分的链路：业务调用先进入 ASU Client，再经过路由、队列、Transport、Provider 发送和远端完成回调。出现时延抖动、任务超时或请求失败时，仅依赖日志和 `kv-test` 最终压测结果，很难快速判断问题发生在 API 提交、Client 排队、Transport 执行、发送还是远端完成阶段。

同时，ASU 有两类实际运行场景：

- standalone：由 `kv-test` 或纯 C++ 宿主直接拉起，不经过 vLLM；
- UCM 集成：ASU 作为 `AsuStore` 的底层实现，由 UCM/vLLM 的 `/metrics` 暴露。

如果两类场景各自维护一套埋点、指标名称和导出方式，将产生口径不一致、Dashboard 无法复用、问题难以横向对比等风险。因此需要以 ASU Metrics 工具为抓手，完成从“只有结果统计”到“可分层定位、可统一观测”的设计改进。

## 三、现状与主要问题

### 3.1 现状

当前已具备以下基础：

- `kv-test` 可输出带宽、IOPS、端到端时延等压测结果；
- UCM 已有 C++ metrics collector 与 Python Prometheus exporter；
- ASU 已有 Client、Transport 等明确的异步任务边界；
- ASU Metrics 工具已形成 facade + backend 的基础实现，支持 standalone HTTP exporter 和 UCM adapter。

### 3.2 改进前的主要痛点

| 序号 | 问题 | 对质量和效率的影响 |
| --- | --- | --- |
| 1 | 压测结果只反映最终结果，无法拆分 Client 排队、Transport 发送和远端完成耗时 | 故障定位依赖人工翻日志，定位周期长 |
| 2 | standalone 与 UCM 的运行依赖不同 | 容易形成两套指标和两套 Dashboard，横向对比困难 |
| 3 | C++ collector 的 drain 接口是消费式读取 | 多个 exporter 并行读取时可能出现指标被先读者取走 |
| 4 | 多个动态库各自静态链接 collector 时会产生多个单例 | 业务已埋点但 exporter 为空，问题隐蔽且难排查 |
| 5 | 单次 `kv-test` 命令生命周期短 | Prometheus 可能来不及抓取，导致“实际有指标但平台无数据” |
| 6 | 指标名称、单位、标签若无统一约束 | 同一概念出现多个口径，告警和趋势分析失真 |

### 3.3 关键原因分析

```text
观测能力不足
├─ 架构：业务埋点与 exporter 强耦合，难兼容不同宿主
├─ 数据：任务、entry、transport task 等统计对象未清晰区分
├─ 运行：短命令退出快；collector/exporer 生命周期不一致
├─ 依赖：共享库实例不唯一，导致数据写入与读取脱节
└─ 展示：指标命名、标签和 Histogram buckets 缺少统一契约
```

## 四、改善目标与验收指标

本课题不预设未经实测的收益数值，采用以下可验收目标；试点前后数据由实际压测和线上观测结果回填。

| 目标项 | 目标值 / 验收标准 |
| --- | --- |
| 双模式覆盖 | standalone 和 UCM 两种模式均可输出 ASU 核心指标 |
| 指标一致性 | 100% 的内置 ASU 指标在两种模式下保持相同基础名、类型、单位和 Histogram buckets |
| 链路可定位性 | 至少可区分 API submit、Client queue、Client process、Transport queue、Send、Completion 六类时延 |
| 导出正确性 | `/metrics` 可输出合法 Prometheus Counter、Gauge、Histogram 文本；Histogram 含 `_bucket`、`_sum`、`_count` |
| 生命周期正确性 | 业务线程停止后执行 final flush；单次命令可通过 grace period 保留抓取窗口 |
| 共享库正确性 | 每个进程中 `libasu_metrics.so.1` 及 UCM 模式的 `libucm_metrics.so.1` 各只加载一份 |
| 回归保障 | standalone exporter、YAML 类型校验、ASU 埋点与 UCM drain 均有自动化测试覆盖 |

## 五、建议方案

### 5.1 总体方案：一套埋点、两种 backend、一个兼容契约

```text
ASU Client / Transport 业务埋点
              │
              ▼
       libasu_metrics.so facade
              │
       ┌──────┴──────┐
       ▼             ▼
Standalone backend  UCM adapter backend
HTTP /metrics       UC::Metrics -> Python exporter -> vLLM /metrics
       └──────┬──────┘
              ▼
        Prometheus / Grafana
```

设计原则：

- 业务代码只调用 ASU Metrics facade，不直接依赖 Prometheus、Python、vLLM 或 UCM；
- 同一进程只初始化一个 backend，避免双写和重复导出；
- `metric_names.h` 统一定义指标基础名、类型和说明；
- `metrics_configs.yaml` 统一定义 exporter 使用的前缀、HELP 文本和 Histogram buckets；
- 通过稳定标签和统一 PromQL，使同一 Dashboard 可覆盖不同运行模式。

### 5.2 重点改进内容

| 改进点 | 改进措施 | 解决的问题 |
| --- | --- | --- |
| 业务与导出解耦 | 引入 `MetricsBackend` 抽象，提供 standalone/UCM 两种实现 | 避免 ASU 业务代码依赖运行宿主 |
| standalone 导出 | 在 `kv-test` 中启动 C++ HTTP exporter，提供 `/metrics` 和 `/health` | 纯 C++ 场景也可被 Prometheus 抓取 |
| 异步链路分段 | 在 Client、Queue、Transport、Send、Completion 处记录阶段时延 | 从“最终慢”提升为“知道慢在哪一段” |
| 单一 drain | standalone 使用独立累计快照；UCM 仅由 Python logger drain | 避免消费者竞争导致空指标 |
| 共享库治理 | collector 使用共享库，部署时校验同进程单例唯一 | 消除“写入和读取不是同一个 collector”风险 |
| 短任务适配 | final flush + `shutdown_grace_ms` | 确保单次 `kv-test` 有被抓取机会 |
| 统一标准 | 固化名称、类型、单位、buckets 与标签规范 | Dashboard、告警和趋势可复用 |

## 六、详细实施措施

### 措施 1：建立统一指标字典

在 `ucm/transport/kv/asu/metrics/include/asu_metrics/metric_names.h` 维护 ASU 内置指标清单，并在 `examples/metrics/metrics_configs.yaml` 维护同名导出配置。

指标按层次划分：

| 层次 | 示例 | 统计口径 |
| --- | --- | --- |
| Client API | `asu_client_store_requests_total` | `StoreAsync` 提交次数 |
| Client API | `asu_client_store_entries_total` | 提交 entry 数，不等同 task 数 |
| Client task | `asu_client_task_queue_duration_seconds` | Client 队列等待时间 |
| Client task | `asu_client_task_duration_seconds` | 从入队到最终完成的端到端时间 |
| Transport task | `asu_transport_task_send_duration_seconds` | 从 transport 分发到 Send 返回的时间 |
| Transport task | `asu_transport_task_completion_duration_seconds` | Send 返回到 completion callback 的时间 |
| Exporter | `asu_metrics_exporter_up` | standalone exporter 存活状态 |

实施要求：新增指标必须同步修改指标字典、YAML、单元测试和 Dashboard 查询；duration 全部使用 seconds；禁止把 key、任务 ID、连接 ID 等动态值拼进指标名或 label。

### 措施 2：在异步任务边界精确埋点

在以下边界记录指标，避免把异步完成时间误记为同步 API 时延：

| 位置 | 建议记录内容 |
| --- | --- |
| `AsuClientImpl::SubmitAsync` | requests、entries、submit duration、提交失败 |
| Client 入队/出队 | enqueue duration、client queue duration |
| `ClientTaskManager` | route/build 与 client process duration |
| `AsuTransportImpl::SubmitTask` | transport queue 进入时间 |
| `TransportTaskExecutor::Execute/Send` | transport queue/process/pre-send/send duration |
| completion 回调 | transport completion duration、client task end-to-end duration |
| `Wait` | wait requests、wait duration、wait errors |

对最终完成路径使用一次性标志保护，保证成功、失败、超时或异常分支不会重复记数。

### 措施 3：完善 standalone collector 与 exporter

standalone backend 采用“线程本地双 buffer + 周期聚合 + 累计快照”的方式：

- 业务线程只更新本线程 buffer，降低高频埋点对 I/O 路径的影响；
- 聚合线程按 `aggregation_interval_ms` 合并增量；
- HTTP 线程只读快照，`GET /metrics` 不触发 drain；
- Counter 以累计值输出，Gauge 以最新值输出，Histogram 累计 bucket/sum/count；
- 支持 `/health` 供部署探活；
- 停止时先关闭 ASU 业务线程，再 final flush、保留可选 grace period、最后关闭 HTTP 服务。

### 措施 4：规范 UCM 集成路径

UCM 模式下，ASU facade 通过 `UcmMetricsBackend` 转发给 `UC::Metrics`，再由 `PrometheusStatsLogger` 周期性调用 `GetAllStatsAndClear()` 并更新 vLLM `/metrics`。

实施要求：

- 不在同一 UCM 进程内启动另一个直接 drain `UC::Metrics` 的 exporter；
- `libasustore.so` 与 `ucmmetrics.so` 必须使用同一份 `libucm_metrics.so.1`；
- AsuStore 对 backend 初始化采用引用计数，避免多个 Store 实例重复初始化或提前关闭；
- 配置文件必须注册全部 ASU 指标，确保 Python exporter 能创建对应 Prometheus 对象。

### 措施 5：统一标签和 Dashboard 使用方式

两种模式至少保持 `model_name`、`worker_id` 标签一致。standalone 额外输出 `source="kv-test"`；现阶段 UCM 路径没有 `source` 标签。

建议：

- 基础 Dashboard 查询仅依赖 `model_name`、`worker_id`；
- 需要区分模式时，短期用 Prometheus 的 `job` 标签；
- 后续将 UCM exporter 统一补充 `source="ucm"` 或 `source="vllm"`，与 standalone 的 `source="kv-test"` 对齐；
- Grafana 统一使用公共指标基础名和相同 Histogram buckets，避免维护两套面板。

### 措施 6：建立验证与回归机制

| 验证层级 | 验证内容 |
| --- | --- |
| 单元测试 | Counter/Gauge/Histogram 累计规则、YAML 名称/类型校验、HTTP 路径和端口失败处理 |
| 并发测试 | 多线程埋点与多次并发 scrape 不丢数、不重复、不互相影响 |
| ASU 集成测试 | Store/Load/Query/Delete/Wait 触发对应 Client/Transport 指标 |
| UCM 集成测试 | ASU 写入后，可被 `ucmmetrics.get_all_stats_and_clear()` drain |
| 部署检查 | `ldd`/运行时日志确认共享库唯一，`curl /metrics` 确认指标存在 |
| 试点验证 | 选择典型 bench 和异常场景，对比改进前后的定位时长与人工排查步骤 |

## 七、实施计划（PDCA）

| 阶段 | 主要工作 | 交付物 |
| --- | --- | --- |
| P：计划 | 梳理 ASU 调用链、定义核心指标和兼容契约 | 指标字典、时序图、改进方案 |
| D：执行 | 开发 facade、standalone backend、UCM adapter 和埋点 | C++ 库、配置、`kv-test` 集成 |
| C：检查 | 执行单测、集成测试、Prometheus/Grafana 验证 | 测试记录、样例 metrics、问题清单 |
| A：处置 | 固化配置、Dashboard、部署检查和新增指标流程 | 开发规范、回归用例、运维手册 |

建议优先级：

1. 先保证 collector 和共享库单例正确；
2. 再落地 Client/Transport 核心时延和错误指标；
3. 完成 standalone `/metrics` 与 UCM/vLLM `/metrics` 验证；
4. 最后补充 Dashboard、告警和试点数据复盘。

## 八、预期效果

完成改进后，ASU metrics 将形成以下能力：

- 在 standalone 和 UCM 两种部署模式下输出同口径核心指标；
- 将一次异步任务拆解为提交、排队、执行、发送、完成等可定位阶段；
- 让 Prometheus/Grafana 能够持续观测 ASU 运行状态，而不仅在压测结束后查看汇总结果；
- 降低“指标未出现”“指标被重复消费”“指标名称不一致”等工程风险；
- 为后续性能优化、异常告警、容量评估和质量回归提供统一数据基础。

建议在试点完成后补充以下实际数据，形成正式 QCC 成果：平均定位时长、重复排查次数、指标覆盖率、抓取成功率、异常场景发现率，以及性能开销评估。

## 九、关联设计资料

技术实现、时序图和两种运行模式的完整兼容说明见：[ASU Metrics 设计：Standalone 与 UCM 统一兼容方案](../user-guide/metrics/asu_metrics_architecture_zh.md)。
