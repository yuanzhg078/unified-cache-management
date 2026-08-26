# ASU kv-test Metrics 与 Grafana 使用手册

本文说明 `kv-test` 使用 standalone metrics 时，Grafana 每条曲线实际测量的代码路径。重点是明确：**图例名、Prometheus 指标、起点、终点，以及数值升高代表什么。**

指标带有 `ucm:` 前缀；Prometheus 的实际 exposition 名会转换为下划线形式，例如 `ucm:asu_client_task_duration_seconds` 会显示为 `ucm_asu_client_task_duration_seconds`。

## 1. 先看完整路径

一次异步 KV 操作大致经过以下阶段：

```text
t0  kv-test 调用 client 的 *Async()
 │
 ├─ client：创建 ClientTask、登记 task、放入 client taskQueue_
 ├─ client：等待 client worker
 ├─ client worker：路由、拆分为一个或多个 TransportTask、逐个 transport->Submit()
 ├─ transport：等待 transport executor
 ├─ transport executor：构造 sub-batch / buffer，准备 provider Send 参数
 ├─ provider->Send() 调用并返回
 ├─ completion worker：收到并处理 CQE，完成每个 TransportTask
 └─ client：聚合所有 child 的完成回调，完成 ClientTask
```

一个 `ClientTask` 可能拆成多个 `TransportTask`。因此 client task 的“全部 child 到达某点”表示**最后一个 child**到达该点，不是所有 child 时间的相加。

## 2. Pre-Send Stage Latency Average

面板 **Pre-Send Stage Latency Average** 是提交到真正调用 `Send()` 之前的拆分图。它使用 Histogram 的 `sum / count`，每个点是查询窗口内的滚动平均值，单位是秒（Grafana 显示为 µs/ms）。

| Grafana 图例 | 指标 | 起点 → 终点 | 数值高通常说明 |
|---|---|---|---|
| `client: API to enqueue` | `ucm:asu_client_task_enqueue_duration_seconds` | 进入 `*Async()` → `ClientTask` 放入 client `taskQueue_` | API 入口同步工作慢：参数/快照检查、创建任务、复制 entry/key 描述符、MR 映射、`taskManager_.Submit()` 或 queue mutex 竞争。不是 timeout 等待。 |
| `client: queue wait` | `ucm:asu_client_task_queue_duration_seconds` | 放入 client `taskQueue_` → client worker 取出 | client worker 忙，client task 在队列堆积。 |
| `client: worker process to transport-submit` | `ucm:asu_client_task_process_duration_seconds` | client worker 取出 → 此 ClientTask 最后一次 `transport->Submit()` 返回 | client 的路由、按 ASU 拆分、创建 child、填回调、循环调用 `transport->Submit()` 慢。**终点是 Submit 返回，不等 provider Send。** |
| `transport: queue wait` | `ucm:asu_transport_task_queue_duration_seconds` | client 即将调用 `transport->Submit()` → transport executor 开始执行该 TransportTask | transport executor 忙，TransportTask 在其队列积压。每个 TransportTask 各记一次。 |
| `transport: executor process to pre-Send` | `ucm:asu_transport_task_process_duration_seconds` | transport executor 开始执行 → 即将调用 provider `Send()` | sub-batch/buffer 构造、连接或请求属性准备等 transport 内部处理慢。 |
| `total: transport task to pre-Send` | `ucm:asu_transport_task_pre_send_duration_seconds` | client 分发该 TransportTask（Submit 前）→ 即将调用 provider `Send()` | 即 transport queue wait + transport executor process；每个 TransportTask 一条样本。 |
| `total: client task to all pre-Send` | `ucm:asu_client_task_pre_send_duration_seconds` | 进入 `*Async()` → 该 ClientTask 的**所有** TransportTask 都到达 provider `Send()` 前 | 整个提交前路径慢；多 child 时由最慢 child 决定。 |

### 一个 ClientTask 只拆成一个 TransportTask 时

下面关系可作为数量级校验，微秒级有少量误差正常：各指标独立取时钟、不同图可能使用相邻但不完全相同的滚动窗口。

```text
client task to all pre-Send
≈ client: API to enqueue
+ client: queue wait
+ client: worker process to transport-submit
+ transport: queue wait
+ transport: executor process to pre-Send

transport task to pre-Send
≈ transport: queue wait
+ transport: executor process to pre-Send
```

如果拆出了多个 TransportTask，不能把“client 总时延”直接减去“transport 平均时延”。client 总时延是到最慢 child 的墙钟时间；transport 面板是所有 child 的样本平均值。

## 3. Send、completion 与完整 task 时延

| Grafana 图例/含义 | 指标 | 起点 → 终点 | 应怎样看 |
|---|---|---|---|
| `client task to all Send return` | `ucm:asu_client_task_send_duration_seconds` | `*Async()` API 入口 → 所有 child 的 provider `Send()` 都返回 | 包含前文全部 pre-Send 路径，及每个 child 的 Send 调用本身；多 child 时由最后返回的 child 决定。 |
| `transport task to Send return` | `ucm:asu_transport_task_send_duration_seconds` | client 分发该 TransportTask（Submit 前）→ 该 task 的 provider `Send()` 返回 | 包含 transport 队列、executor 处理与 Send 调用。它不是“纯 Send 函数耗时”。 |
| `transport task: Send return to completion` | `ucm:asu_transport_task_completion_duration_seconds` | 对该 TransportTask 的 `Send()` 返回 → completion worker 收到/处理 CQE 并触发 transport 完成回调 | Send 已返回后仍慢，重点检查 CQE、provider/后端返回与 completion 路径。一个 TransportTask 的全部 sub-batch 完成后才算完成。 |
| `client task end-to-end` | `ucm:asu_client_task_duration_seconds` | `*Async()` API 入口 → 所有 TransportTask 完成回调被 client 聚合 | 一个 ClientTask 的完整本地端到端时延；包含 Send 前、Send、completion 与最终聚合。 |

常用诊断：

```text
client task to all pre-Send 高
  → client/transport 的排队、路由、拆分或准备阶段慢

transport task to Send return 高，但 transport task to pre-Send 正常
  → provider Send 调用本身或其同步阻塞部分慢

Send return to completion 高
  → Send 已完成，慢在 CQE、后端返回、poll/completion worker

client end-to-end 高，而 completion 不高
  → 对照 client task to all Send return，检查 client 侧的聚合或多 child 最慢分支
```

这些都是 client/transport 进程视角的时延，不能替代 ASU 后端服务自己的队列、执行和存储时延；后端要单独暴露 metrics。

## 4. 接口级 submit / Wait 时延

`Task Pipeline` 是内部 task 路径；接口级 Histogram 回答的是“调用 API 到异步提交返回有多快”。

| 图例类别 | 指标 | 起点 → 终点 |
|---|---|---|
| Query submit | `ucm:asu_client_query_submit_duration_seconds` | `QueryAsync()` 进入 → `SubmitAsync` 返回 |
| Load submit | `ucm:asu_client_load_submit_duration_seconds` | `LoadAsync()` 进入 → `SubmitAsync` 返回 |
| Store submit | `ucm:asu_client_store_submit_duration_seconds` | `StoreAsync()` 进入 → `SubmitAsync` 返回 |
| BatchLoad submit | `ucm:asu_client_batch_load_submit_duration_seconds` | `BatchLoadAsync()` 进入 → `SubmitAsync` 返回 |
| BatchStore submit | `ucm:asu_client_batch_store_submit_duration_seconds` | `BatchStoreAsync()` 进入 → `SubmitAsync` 返回 |
| Delete submit | `ucm:asu_client_delete_submit_duration_seconds` | `DeleteAsync()` 进入 → `SubmitAsync` 返回 |
| Wait | `ucm:asu_client_wait_duration_seconds` | 进入 `Wait()` → Wait 返回 |

`*_submit_duration_seconds` 不等后端实际完成，通常只是异步 API 交付 task id/提交工作的耗时。`Wait` 才是调用方等待 task completion 的时长，但它也只覆盖该次 `Wait()` 调用：若 task 在 Wait 前就已完成，Wait 可以很短。

## 5. 非时延指标

除 `Wait` 外，Query、Load、Store、BatchLoad、BatchStore、Delete 都有以下三类 Counter：

| 类别 | 形式 | 意义 | 常用 Grafana / PromQL 解读 |
|---|---|---|---|
| 请求数 | `ucm:asu_client_<operation>_requests_total` | 对该异步接口的调用次数 | `rate(...[$__rate_interval])` 是请求/s。 |
| entry/key 数 | `ucm:asu_client_<operation>_entries_total` | 该操作涉及的 entry 或 key 总数 | `entries rate / requests rate` 是平均 batch 大小；单条 Store/Load 通常接近 1。 |
| 错误数 | `ucm:asu_client_<operation>_errors_total` | 该操作失败返回的累计次数 | `rate(errors) / rate(requests)` 是错误率。持续非零或突增应结合 kv-test 日志排查。 |

`Wait` 没有 entries 指标，只有：

```text
ucm:asu_client_wait_requests_total
ucm:asu_client_wait_errors_total
ucm:asu_client_wait_duration_seconds
```

例如 `client_task_complete` 或 task completion 语义的计数，应该按 **task 数**理解，不是 BatchStore 内 entry 的数量；要看 entry 数应使用对应 operation 的 `*_entries_total`。

### Exporter 自身

| 指标 | 含义 | 注意 |
|---|---|---|
| `ucm:asu_metrics_exporter_up` | exporter 在最近一次暴露 metrics 时写出的内部状态 | 它最后一次抓到 1 后，短命 `kv-test` 退出也可能仍显示 1；不能作为实时进程存活判断。 |
| `ucm:asu_metrics_exporter_http_requests_total` | exporter HTTP 请求累计次数 | 可确认 `/metrics` / `/health` 是否曾被访问；不是业务请求数。 |
| Prometheus 原生 `up{job="...",instance="..."}` | Prometheus 当前是否能 scrape 到目标 | 实时存活看这个：1 可抓取，0 表示目标仍配置但当前不可达。 |

## 6. Histogram、平均值、P50/P99 是什么

每个 `*_duration_seconds` 都是 Prometheus Histogram，会导出：

```text
..._bucket  各延迟阈值以内的累计样本数
..._sum     全部样本耗时总和（秒）
..._count   全部样本数量
```

Dashboard 的两种时延图不是同一种统计：

| 面板 | 算法 | 回答的问题 |
|---|---|---|
| `Task Pipeline Latency P99 / P50` | `histogram_quantile()` 读取 `_bucket` | P50 是典型 task 多快；P99 是最慢约 1% task 有多慢。P99 明显高于 P50，说明有长尾/偶发排队。 |
| `Task Pipeline Latency Average`、`Pre-Send Stage Latency Average` | `rate(_sum) / rate(_count)` | 查询窗口内所有样本的平均时延；容易被少量极慢或极快样本拉动，不能替代 P99。 |
| `Task Pipeline Observation Counts` | `_count` | 当前窗口内有多少样本。count 不增时，平均与分位数应视为无数据，而不是 0 时延。 |

平均值的标准表达式为：

```promql
sum(rate(ucm_asu_client_task_duration_seconds_sum[$__rate_interval]))
/
sum(rate(ucm_asu_client_task_duration_seconds_count[$__rate_interval]))
```

分位数例如 P99：

```promql
histogram_quantile(0.99,
  sum by (le) (
    rate(ucm_asu_client_task_duration_seconds_bucket[$__rate_interval])
  )
)
```

Grafana 时间范围不是“一轮 bench 的平均范围”。每个折线点使用 `$__rate_interval` 的**滚动窗口**；该窗口通常至少约为 Prometheus scrape interval 的 4 倍。使用 scrape=2s 时常见窗口约为 8s 或更大。请同时查看 Observation Counts，确认该滚动窗口确实包含本轮样本。

## 7. Dashboard 面板对应关系

模板位于 `examples/metrics/grafana_asu_client.json`。

| 面板 | 主要指标 | 用途 |
|---|---|---|
| `Pre-Send Stage Latency Average` | 本文第 2 节 7 个指标的 `_sum/_count` | 优先定位 API 入队、client 队列、client worker、transport 队列还是 transport 准备阶段慢。 |
| `Task Pipeline Latency P99 / P50` | client pre-Send、transport pre-Send、client Send、transport Send、transport completion、client end-to-end 的 `_bucket` | 看典型与长尾；应同时展示 client 与 transport，避免只看 client Send 而无法判断队列或发送阶段。 |
| `Task Pipeline Latency Average` | 上述 pipeline 指标的 `_sum/_count` | 与 P50/P99 对照，查看整体平均成本。 |
| `Task Pipeline Observation Counts` | pipeline 的 `_count` | 检查每一段是否真的有点位；短命 bench 样本少时尤其重要。 |
| `Submission Rate by Operation` | 各操作 `*_requests_total` | 请求 QPS。 |
| `Entry Throughput` | 各操作 `*_entries_total` | entry/key 吞吐；请求 QPS 的倍数反映 batch 大小。 |
| `Store / Load`、`Batch / Wait`、`Query / Delete` Submit Latency Quantiles | 对应接口时延 Histogram 的 `_bucket` | API 提交耗时或 Wait 的长尾。不要把 submit 图误当端到端完成时延。 |
| `Error Rate by Operation` | `*_errors_total / *_requests_total` | 失败比例。 |
| `Raw ... Counters` / `Raw Histogram Observation Counts` | Counter 与 `_count` 原始值 | 核验短测是否真的写入过指标。 |

## 8. kv-test 短测和实时观察建议

高并发 bench 的实用组合：

```properties
metrics.aggregation_interval_ms=1000   # 1000 推荐；更在意低开销可用 2000
metrics.shutdown_grace_ms=30000        # 短命 bench 结束后留给 Prometheus 最后一抓
```

并配合：Prometheus scrape interval = 2s，Grafana refresh = 5s，Grafana datasource 的 Scrape interval 也填 2s。

`aggregation_interval_ms` 不会采样或丢掉每次 Observe：每个写点先累积在写入线程的 buffer，聚合周期只影响多久合并到 exporter snapshot。它只影响面板新鲜度与极小的周期性聚合开销。若设置为 2s、scrape 为 2s、Grafana 刷新 5s，图上看到最新点可能有数秒延迟是正常的。

每次新启动 `kv-test` 是新进程，Counter 会从 0 重新开始；Grafana 不会自动删除 Prometheus 中旧进程的历史样本。重新测一轮时，选择测量开始后的短时间范围，并用 `source`、`model_name`、`worker_id` 筛选。若多轮 bench 使用完全相同的标签，曲线会按时间连续显示；要并排比较不同并发/批大小，应给每轮使用不同 `worker_id` 或 `source`，或者另行增加 run 标签。

## 9. 快速排障顺序

1. 先看 `Task Pipeline Observation Counts` 是否增长，避免把“无样本”误判为 0 时延。
2. 看 `Pre-Send Stage Latency Average`：先确定慢在 client 入队、client queue、client worker、transport queue 或 transport executor。
3. 再看 Pipeline P50/P99：P99 突增而平均正常，通常是偶发排队；P50/P99 同时升高，通常是整体饱和或处理变慢。
4. 若 pre-Send 正常但 Send return 高，检查 provider Send；若 completion 高，检查 CQE、后端返回和 completion worker。
5. 最后结合 `Error Rate`、QPS、entry 吞吐与 kv-test 日志，判断是否为错误、限流或实际负载变化。

## 10. 标签

每条 standalone 指标带有：

```text
source      # 例如 kv-test
model_name  # 例如 standalone
worker_id   # 例如 asu-0
```

Grafana 顶部的 Source、Model、Worker 变量就是这些标签。多实例同时写入时先筛选标签，避免把不同进程的 Counter、Histogram 混合。`bench.concurrency`、`bench.batch_size` 等参数当前不是 metrics label；同一标签下的不同测试轮次不能由 Dashboard 自动区分。
