# ASU kv-test Metrics 点位与 Grafana 解读

本文只说明 ASU client 在 standalone / kv-test 模式下采集了哪些指标，以及在 Grafana 中如何解读这些指标。所有指标由 ASU client 埋点，名称均带有 Prometheus 前缀 `ucm:`。

## 1. 点位总览

ASU client 对下列七种操作采集指标：

```text
Query、Load、Store、BatchLoad、BatchStore、Delete、Wait
```

除 `Wait` 外，每种操作都有四类点位：请求次数、entry/key 数、错误次数、提交耗时。`Wait` 没有 entry/key 的概念，因此只有请求、错误和等待耗时。

| 操作 | 请求次数 | entries / keys | 错误次数 | 耗时 |
|---|---|---|---|---|
| Query | `ucm:asu_client_query_requests_total` | `ucm:asu_client_query_entries_total` | `ucm:asu_client_query_errors_total` | `ucm:asu_client_query_submit_duration_seconds` |
| Load | `ucm:asu_client_load_requests_total` | `ucm:asu_client_load_entries_total` | `ucm:asu_client_load_errors_total` | `ucm:asu_client_load_submit_duration_seconds` |
| Store | `ucm:asu_client_store_requests_total` | `ucm:asu_client_store_entries_total` | `ucm:asu_client_store_errors_total` | `ucm:asu_client_store_submit_duration_seconds` |
| BatchLoad | `ucm:asu_client_batch_load_requests_total` | `ucm:asu_client_batch_load_entries_total` | `ucm:asu_client_batch_load_errors_total` | `ucm:asu_client_batch_load_submit_duration_seconds` |
| BatchStore | `ucm:asu_client_batch_store_requests_total` | `ucm:asu_client_batch_store_entries_total` | `ucm:asu_client_batch_store_errors_total` | `ucm:asu_client_batch_store_submit_duration_seconds` |
| Delete | `ucm:asu_client_delete_requests_total` | `ucm:asu_client_delete_entries_total` | `ucm:asu_client_delete_errors_total` | `ucm:asu_client_delete_submit_duration_seconds` |
| Wait | `ucm:asu_client_wait_requests_total` | — | `ucm:asu_client_wait_errors_total` | `ucm:asu_client_wait_duration_seconds` |

此外还有 exporter 自身的两个点位：

```text
ucm:asu_metrics_exporter_up
ucm:asu_metrics_exporter_http_requests_total
```

`asu_metrics_exporter_up=1` 表示 standalone metrics exporter 正在运行；`http_requests_total` 是 `/metrics`、`/health` 等 HTTP 请求的累计服务次数。

## 2. 每类点位是什么意思

### 2.1 `*_requests_total`

这是某种 ASU client 操作成功提交或失败返回的总调用次数。例如：

```text
asu_client_store_requests_total = 1000
```

表示当前进程启动以来共执行了 1,000 次 Store 提交。

它是 Counter，只会增加。Grafana 一般不直接看其绝对值，而是使用 `rate()` 显示每秒请求数（QPS）。

### 2.2 `*_entries_total`

这是对应操作累计处理的 entry 或 key 数。单条 Store/Load 通常是 1；BatchStore/BatchLoad 中它可以远大于请求数。

```text
batch_store_requests_total = 100
batch_store_entries_total  = 10000
```

表示执行了 100 个 BatchStore 请求，平均每批约 100 个 entry：

```text
平均 batch 大小 = entries_total / requests_total = 100
```

### 2.3 `*_errors_total`

这是该操作的累计失败次数。它也是 Counter。

```text
错误率 = rate(errors_total) / rate(requests_total)
```

正常情况下错误率应接近 0。错误数上升时，先看哪个操作的 `errors_total` 增长，再结合 kv-test 日志定位具体失败原因。

### 2.4 `*_submit_duration_seconds`

这是 Histogram，记录各操作的 **客户端提交耗时**。

对于 Query、Load、Store、BatchLoad、BatchStore、Delete：

```text
进入 ASU client 方法
  → 调用 SubmitAsync
  → SubmitAsync 返回
```

因此它衡量的是 ASU client 将请求提交给底层 transport 的开销；**不是** 服务端处理结束或数据真正落盘的端到端耗时。

`asu_client_wait_duration_seconds` 的含义不同：它记录 `Wait` 的实际等待时长，更接近异步请求等待 completion 返回的时间。

### 2.5 Task 与 transport 分段指标

除接口提交和 `Wait` 外，standalone metrics 还提供 client 内部任务路径的聚合指标：

| 指标 | 含义 |
|---|---|
| `ucm:asu_client_task_queue_duration_seconds` | task 从进入 client 队列到 client worker 开始处理的等待时间。持续升高表示 client 侧排队。 |
| `ucm:asu_client_task_duration_seconds` | task 从入队到所有 transport task 回调聚合完成的端到端时间。 |
| `ucm:asu_client_task_transport_fanout` | 一个 client task 按路由拆出的 transport task 数；数值升高表示一次请求分散到了更多 ASU。 |
| `ucm:asu_client_tasks_completed_total` / `ucm:asu_client_task_errors_total` | client task 的完成和失败总数。 |
| `ucm:asu_transport_task_duration_seconds` | 单个 transport task 从 client 分发到收到完成回调的时长，包含 transport 排队、发送、等待 CQE 和回调路径。 |
| `ucm:asu_transport_tasks_completed_total` / `ucm:asu_transport_task_errors_total` | transport task 的完成和失败总数。 |

这些指标仍由 client/transport 进程观测，不能代替 ASU 服务端的真实执行耗时。服务端队列和存储执行耗时需要由 ASU 后端单独暴露 metrics。

## 3. Histogram 怎么看

每个 `*_duration_seconds` 在 Prometheus 中会展开成三组序列：

```text
..._bucket  # 各延迟桶内的累计观测数
..._sum     # 全部耗时之和，单位秒
..._count   # 总观测次数
```

例子：

```text
asu_client_load_submit_duration_seconds_sum   = 3.91e-06
asu_client_load_submit_duration_seconds_count = 1
```

说明记录到一次 Load 提交，耗时约 3.91 微秒。

Grafana 中不直接画所有 bucket，因为 bucket 过多且可读性差；应使用 bucket 计算延迟分位数：

```text
P50：典型请求的提交耗时
P95：95% 请求不超过的提交耗时
P99：长尾请求的提交耗时
```

判断原则：

| 现象 | 含义 |
|---|---|
| P50、P95、P99 接近 | 提交延迟分布稳定 |
| P99 明显高于 P50 | 存在长尾，少量请求提交明显更慢 |
| P50/P95/P99 同时上升 | 底层 transport、线程调度或资源竞争整体变慢 |
| `*_count` 不增长 | 该操作没有执行，或对应埋点路径没有走到 |

## 4. Grafana 模板中的面板与点位对应关系

Dashboard 模板：`examples/metrics/grafana_asu_client.json`。

| Grafana 面板 | 使用的点位 | 看什么 |
|---|---|---|
| ASU Metrics Exporter Up | `asu_metrics_exporter_up` | 是否成功启动并保持运行。值应为 1 |
| Submission Rate by Operation | 所有 `*_requests_total`（不含 Wait） | 每个操作的提交 QPS；用于比较读写、批量和删除负载 |
| Entry Throughput | 所有 `*_entries_total`（不含 Wait） | 每秒处理的 entry/key 数；Batch 操作可据此判断实际批量大小 |
| Store / Load Submit Latency Quantiles | Store / Load 的 `*_submit_duration_seconds_bucket` | Store、Load 的 P50/P95/P99 提交延迟 |
| Batch / Wait Latency Quantiles | BatchStore、BatchLoad、Wait 的 `*_duration_seconds_bucket` | 批量提交延迟和 completion 等待长尾 |
| Query / Delete Submit Latency Quantiles | Query、Delete 的 `*_submit_duration_seconds_bucket` | Query、Delete 的 P50/P95/P99 提交延迟 |
| Error Rate by Operation | 所有 `*_errors_total`、`*_requests_total` | 各操作错误率；应优先关注持续非零或突增 |
| Wait and Exporter Activity | Wait 请求/错误、`asu_metrics_exporter_http_requests_total` | completion 等待负载和 exporter 被读取情况 |
| Raw Request Counters | 所有 `*_requests_total` | 原始累计请求数，用于核验各操作是否实际执行 |
| Raw Entry Counters | 所有 `*_entries_total` | 原始累计 entry/key 数，用于核验批量语义 |
| Raw Error Counters | 所有 `*_errors_total` | 原始累计错误数，用于确认具体失败的操作 |
| Raw Histogram Observation Counts | 所有 `*_duration_seconds_count` | 每个耗时 Histogram 是否记录到了样本；通常应与对应请求次数一致 |
| Task Pipeline Latency | task queue、client task、transport task 的 duration buckets | 并排看 client 排队、task 端到端和 transport/CQE 路径的 P95、P99，定位延迟发生的层级。 |
| Transport Task Fanout | `asu_client_task_transport_fanout_bucket` | 每个 client task 被路由拆成的 transport task 数；P95 上升表示请求跨更多 ASU。 |
| Task Completion Rate | client / transport task completed counters | client task 与 transport task 的每秒完成数量。 |
| Task Error Rate | client / transport task error 与 completed counters | 区分 client 路由/聚合失败和 transport/CQE 路径失败。 |
| Raw Task / Transport Counters | task completed/error counters | 排查短命 benchmark 时的原始 task 完成及错误数。 |

## 5. 看图时的常见判断

### Store / Load

```text
Store QPS 上升，Store entries QPS 同比例上升
→ 单条 Store 为主，平均每请求约一个 entry

BatchStore entries QPS 远高于 BatchStore 请求 QPS
→ 批处理正在生效；两者之比约为平均 batch 大小

Store P99 上升，但 Wait P99 没上升
→ 更可能是提交侧/transport 入队侧变慢

Store submit P99 平稳，但 Wait P99 上升
→ 请求提交正常，完成路径或后端处理变慢
```

### 错误

```text
errors_total 增长且 requests_total 同时增长
→ 有真实业务请求失败，应看对应 kv-test/ASU 日志

errors_total 不增长，但 Error Rate 面板偶尔出现空值
→ 请求量为零时没有可计算的 rate，通常不是错误
```

### 单次 `store --check`

`store --check` 会 Store 后再 Load 回读验证。因此原始 Counter 常见预期为：

```text
store_requests_total = 1
store_entries_total  = 1
load_requests_total  = 1
load_entries_total   = 1
store_errors_total   = 0
load_errors_total    = 0
```

短命的单次进程常没有连续的 QPS 曲线，因为 `rate()` 至少需要多个采样点。此时优先看 Raw Request Counters、Raw Entry Counters、Raw Error Counters、Raw Histogram Observation Counts；持续 `kv-test bench` 才适合分析 QPS、P95、P99 曲线。

## 6. 标签

每条 ASU standalone 指标带有：

```text
source      # 例如 kv-test
model_name  # 例如 standalone
worker_id   # 例如 asu-0
```

Grafana 顶部的 Source、Model、Worker 变量正是这些标签。多个 kv-test 实例、多个 ASU worker 同时写 Prometheus 时，先用标签筛选再判断数值，避免把不同实例的 Counter 或延迟聚合在一起。
