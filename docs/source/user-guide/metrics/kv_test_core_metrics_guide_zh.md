# kv-test 必要 Metrics 与 Grafana 指南

本文只说明 `kv-test` 客户端测试链路的必要打点，不包含 ASU Store 服务端指标。配套 Dashboard：

```text
examples/metrics/grafana_kv_client.json
```

指标实际名称保留 `kv:` 前缀，例如 `kv:kv_client_task_e2e_duration_seconds`。

异步任务在各阶段直接记录时间点，时间点保存在 ClientTask / TransportTask 中，以便跨线程计算时延。即使 metrics 未启用，仍会取时间并执行 client 阶段回调；`UpdateStats()` 检查后端，未启用时不写入指标。若在任务执行过程中切换 metrics，可能只记录部分阶段。原有 `MetricTimer`、`StartMetricTimer()`、`ElapsedSeconds()` 接口保留，供其他计时流程使用。

## 1. 选择原则

必要点位必须能回答四个问题：

1. 本轮测试提交了多少请求和 entry/key？
2. 提交是否失败，最终 Wait 是否失败？
3. 时延主要消耗在 client、transport、Send 还是 completion？
4. 当前图上的平均值和分位数是否确实有样本？

必要集合共引用 36 个指标：20 个请求/entry/error Counter 和 16 个时延 Histogram。其中 4 个 Histogram 专用于 Fake provider 与 completion 后半段的细分定位。

## 2. 请求量、吞吐与错误

下面的面板名和图例名与 `grafana_kv_client.json` 完全一致。顶部 Stat 没有折线图例，因此使用 Stat 面板标题标识。

### 2.1 请求 Counter

Dashboard 面板：`Request Rate by Operation`。

| 指标 | 图例 | 含义 |
|---|---|---|
| `kv:kv_client_store_requests_total` | `store` | Store API 调用次数 |
| `kv:kv_client_load_requests_total` | `load` | Load API 调用次数 |
| `kv:kv_client_batch_store_requests_total` | `batch-store` | BatchStore API 调用次数 |
| `kv:kv_client_batch_load_requests_total` | `batch-load` | BatchLoad API 调用次数 |
| `kv:kv_client_query_requests_total` | `query` | Query API 调用次数 |
| `kv:kv_client_delete_requests_total` | `delete` | Delete API 调用次数 |

以上六个 Counter 还会聚合到顶部 `Total Request Rate` Stat。

### 2.2 Entry/key Counter

Dashboard 面板：`Entry / Key Throughput`。

| 指标 | 图例 | 含义 |
|---|---|---|
| `kv:kv_client_store_entries_total` | `store entries` | 传给 `StoreAsync()` 的 `KVBuffer` 数量；kv-test bench 每次为 1 |
| `kv:kv_client_load_entries_total` | `load entries` | 传给 `LoadAsync()` 的 `KVBuffer` 数量；kv-test bench 每次为 1 |
| `kv:kv_client_batch_store_entries_total` | `batch-store entries` | 传给 `BatchStoreAsync()` 的 `KVBuffer` 数量；kv-test bench 每次为 `bench.batch_size` |
| `kv:kv_client_batch_load_entries_total` | `batch-load entries` | 传给 `BatchLoadAsync()` 的 `KVBuffer` 数量；kv-test bench 每次为 `bench.batch_size` |
| `kv:kv_client_query_entries_total` | `query keys` | Query key 数量 |
| `kv:kv_client_delete_entries_total` | `delete keys` | Delete key 数量 |

以上六个 Counter 还会聚合到顶部 `Entry / Key Throughput` Stat。

这里的一个 entry 就是一个 KV API 输入元素，不是模型层数。kv-test 会为每个 entry 生成一个 key 和一段大小为 `bench.io_size` 的 value buffer，不读取 `use_layerwise`，也不会把 block 自动乘以 layer 数。因此在本文限定的 kv-test 场景中：

```text
Store entries 增量      = 1 × Store API 调用次数
BatchStore entries 增量 = 本次 batch_size
测试字节数              = entry 数 × bench.io_size
```

### 2.3 提交错误 Counter

Dashboard 面板：`Submission and Final-result Error Rate`。

| 指标 | 图例 | 含义 |
|---|---|---|
| `kv:kv_client_store_errors_total` | `store submit` | Store API 提交失败次数 |
| `kv:kv_client_load_errors_total` | `load submit` | Load API 提交失败次数 |
| `kv:kv_client_batch_store_errors_total` | `batch-store submit` | BatchStore API 提交失败次数 |
| `kv:kv_client_batch_load_errors_total` | `batch-load submit` | BatchLoad API 提交失败次数 |
| `kv:kv_client_query_errors_total` | `query submit` | Query API 提交失败次数 |
| `kv:kv_client_delete_errors_total` | `delete submit` | Delete API 提交失败次数 |

`*_errors_total` 只表示 `*Async()` 提交失败，例如参数错误、未初始化或队列已满，不表示后端异步执行的最终结果。

### 2.4 Wait 点位

展示位置：`Submission and Final-result Error Rate`。

| 指标 | 图例或 Stat | 含义 |
|---|---|---|
| `kv:kv_client_wait_requests_total` | `wait/final result` 的分母 | `Wait()` 调用次数 |
| `kv:kv_client_wait_errors_total` | `wait/final result`；错误区的 `Final Error Rate` | `Wait()` 或 task 最终结果失败次数 |

Dashboard 中的最终结果错误率为：

```promql
rate(kv_client_wait_errors_total)
/
rate(kv_client_wait_requests_total)
```

它表示 **Wait 调用的失败比例**，不是所有 API 提交请求的失败比例。每次 `Wait()` 返回时，`kv_client_wait_requests_total` 加 1；如果 `Wait()` 自身失败，或者返回的 `TaskResult.status` 失败，`kv_client_wait_errors_total` 同时加 1。提交阶段已经失败、因而没有调用 `Wait()` 的请求不进入这个分母。

kv-test bench 通常对每个成功提交的 task 调用一次 `Wait()`，因此这里通常可以理解为“成功提交 task 的最终失败率”。但指标本身按 Wait 调用计数：如果同一个 task 被 Wait 多次，也会计数多次。

任务时延统一使用 `kv_client_task_e2e_duration_seconds`，避免把可能在 task 完成后才调用的 `Wait()` 阻塞时间误解为端到端时延。

## 3. 必要时延点位

本文所说的“API 入口”具体指本次 ClientTask 进入 `KvClientImpl::SubmitAsync()` 后记录 `taskStart` 的位置。外层异步提交接口包括：

- `QueryAsync()`
- `LoadAsync()`
- `StoreAsync()`
- `BatchLoadAsync()`
- `BatchStoreAsync()`
- `DeleteAsync()`

例如 kv-test 调用 `BatchStoreAsync()`，该 ClientTask 的起点就是其进入内部 `KvClientImpl::SubmitAsync()` 后记录 `taskStart` 的位置。外层包装函数到 `SubmitAsync()` 入口之间的极小调用开销不计入任务时延。`Wait()` 不是这个时延的起点，也不包含在 `kv_client_task_e2e_duration_seconds` 中。

一次 task 的主要路径：

```text
API entry
  │
  ├─ API 创建并登记 task
  │    kv_client_task_enqueue_duration_seconds
  │
  ├─ client taskQueue 等待
  │    kv_client_task_queue_duration_seconds
  │
  ├─ client worker 路由、拆分、提交 transport task
  │    kv_client_task_process_duration_seconds
  │
  ├─ transport executeQueue 等待
  │    kv_transport_task_queue_duration_seconds
  │
  ├─ transport executor 构造 sub-batch/buffer
  │    kv_transport_task_process_duration_seconds
  │
  ├─ provider Send() 调用并返回
  │
  ├─ completion worker 收到并处理 CQE
  │    kv_transport_task_completion_duration_seconds
  │    kv_transport_task_e2e_duration_seconds（transport submit → completed）
  │
  └─ client 聚合 child，task 完成
       kv_client_task_e2e_duration_seconds
```

### 3.1 五个必要阶段

Dashboard 面板：`Pre-Send Stage Breakdown - Average`。

| 指标 | 图例 | 起点 → 终点 | 升高通常说明 |
|---|---|---|---|
| `kv:kv_client_task_enqueue_duration_seconds` | `1 API → client queue` | 内部 `SubmitAsync()` 入口 → 成功入队前记录的 `enqueuedAt` | 创建 task、复制描述符、MR 映射、task manager 或 producer mutex 慢 |
| `kv:kv_client_task_queue_duration_seconds` | `2 client queue wait` | 同一个 `enqueuedAt` → client worker 开始处理 | `TryPush()` 开销、client worker 饱和或排队 |
| `kv:kv_client_task_process_duration_seconds` | `3 client process` | client worker 取出 → 所有 `transport->Submit()` 返回 | 路由、拆分、创建 child 或 transport 提交慢 |
| `kv:kv_transport_task_queue_duration_seconds` | `4 transport queue wait` | transport task 提交 → executor 取出 | transport executor 饱和或排队 |
| `kv:kv_transport_task_process_duration_seconds` | `5 transport process` | executor 取出 → 调用 provider `Send()` 前 | sub-batch、buffer、连接和请求属性准备慢 |

client 的前三段以同一任务的 `enqueuedAt`、`processingStartedAt` 为共享边界；成功到达对应阶段时，`enqueue + queue = SubmitAsync() 入口 → client worker 开始处理`。`enqueuedAt` 必须在 `TryPush()` 发布任务前写入，因此少量 `TryPush()` 开销归入 queue 段。transport 指标按 child TransportTask 采样；一个 ClientTask 可拆成多个 child，且 child 提交可能与 client process 阶段重叠，所以不能把五个指标的聚合平均值直接相加。

### 3.2 必要边界点位

主要展示位置：`Required Pipeline Boundaries - Average/P50/P99`；完成吞吐出现在 `Task Completion Rate`。

#### ClientTask：优先观察的三条线

每个样本代表一次 client API task；如果该 task 被拆成多个 child，边界由最后一个 child 决定。

| 代码指标 | Dashboard 显示名称 | 起点 → 终点 | Grafana 用途 |
|---|---|---|---|
| `kv:kv_client_task_pre_send_duration_seconds` | `ClientTask: API → all children pre-Send` | API 入口 → 所有 child 到达 pre-Send | 判断请求在真正发给 provider 以前是否已经变慢 |
| `kv:kv_client_task_send_duration_seconds` | `ClientTask: API → all Send returned` | API 入口 → 所有 child 的 `Send()` 返回 | 与上一条对照，判断 provider `Send()` 调用阶段是否异常 |
| `kv:kv_client_task_e2e_duration_seconds` | `ClientTask E2E: API → completed` | API 入口 → 所有 child 完成并由 client 聚合 | client task 端到端时延，是最重要的一条线；P99 还显示在顶部 `Client Task End-to-End P99` Stat |

#### TransportTask：定位 child 长尾的四条线

每个样本代表一个发往单个路由目标的 TransportTask，不等同于一次 client API task。

| 代码指标 | Dashboard 显示名称 | 起点 → 终点 | Grafana 用途 |
|---|---|---|---|
| `kv:kv_transport_task_pre_send_duration_seconds` | `TransportTask: submit → pre-Send` | transport task 提交 → provider `Send()` 前 | 判断某个 child 是否慢在 transport 排队和发送准备 |
| `kv:kv_transport_task_send_duration_seconds` | `TransportTask: submit → Send returned` | transport task 提交 → provider `Send()` 返回 | 与上一条对照，判断 child 的 `Send()` 调用阶段是否异常 |
| `kv:kv_transport_task_completion_duration_seconds` | `TransportTask: Send returned → completed` | provider `Send()` 返回 → CQE 处理并触发完成 | 判断 child 是否慢在后端异步完成、CQE 等待或 poll 调度 |
| `kv:kv_transport_task_e2e_duration_seconds` | `TransportTask E2E: submit → completed` | transport task 提交 → 所有 sub-batch 完成并触发完成 | TransportTask 的完整端到端时延；其 `_count` 在 `Task Completion Rate` 中显示为 `transport tasks completed` |

`Required Pipeline Boundaries - Average/P50/P99` 使用同一组点位，分别展示整体均值、典型值和长尾；它们不是重复打点。

一个 ClientTask 可能拆成多个 TransportTask。client 边界指标表示最后一个 child 到达边界的墙钟时间，不能与 transport 样本平均值直接相减。

`kv_transport_task_send_duration_seconds` 不是纯 `Send()` 函数时延，它还包含 transport queue 和 executor process。当前只能通过它与 pre-Send 指标对照判断 Send 阶段是否异常。

若 TransportTask 在调用 provider `Send()` 前失败或未实际提交，不记录其 `send` / `completion` 阶段时延；若任一 child 未到达 `Send()` 返回，整个 ClientTask 也不记录 `kv_client_task_send_duration_seconds`。这些任务仍可计入相应的 E2E 指标。

### 3.3 `ClientTask: API → all children pre-Send` 中的 child

这里的 child 指 TransportTask。一个 ClientTask 中的 entries/keys 会先按路由目标 `nodeId` 分组，每个目标节点生成一个 TransportTask：

```text
一个 ClientTask
  ├─ node A 的 entries → TransportTask A
  ├─ node B 的 entries → TransportTask B
  └─ node C 的 entries → TransportTask C
```

代码位置：

```text
kv_semantics/src/client/task/task_manager.cc
  BuildTransportTasks()  # RouteKeys 后按 nodeId 创建 TransportTask
  DispatchTask()         # 为每个 TransportTask 注册 onPreSend 回调
```

创建完成后，`remainingTransportPreSendTasks` 被初始化为 TransportTask 数量。每个 TransportTask 完成 queue wait、sub-batch/buffer 构造和连接准备，在真正调用 provider `Send()` 前执行 `NotifyPreSend()`，使计数器减 1。最后一个 child 将计数器从 1 减到 0 时，记录：

```text
kv_client_task_pre_send_duration_seconds
= 当前时间 - ClientTask submittedAt
```

因此该指标表示从 API 入口到最慢 child 到达 pre-Send 的墙钟时间：

```text
client to all pre-Send = max(各 child 到达 pre-Send 的时间点) - API 入口时间
```

它不是各 child 时延之和。若所有 entries 都路由到同一个节点，ClientTask 通常只有一个 TransportTask，此时 “all child” 就是这一个 child。TransportTask 后续还可能拆成多个 provider sub-batch，但 `remainingTransportPreSendTasks` 统计的是 TransportTask，不是 sub-batch。

### 3.4 Completion 与端到端时序

#### 单个 TransportTask

`kv_transport_task_completion_duration_seconds` 从 provider `Send()` 返回后开始计时，到 completion worker 确认这个 TransportTask 的所有 sub-batch 已完成时结束：

```text
TransportTask
    │
    ├─ transport queue wait
    ├─ executor 构造 sub-batch / buffer
    ├─ provider Send()
    │        │
    │        └─ Send 返回，记录 sendCompletedAt
    │                     │
    │                     │  kv_transport_task_completion_duration_seconds
    │                     │
    ├─ 等待后端处理和 CQE 就绪
    ├─ completion worker Poll
    ├─ 处理各 sub-batch 的 CQE
    └─ 所有 sub-batch 完成，记录 completion duration
             │
             └─ 构造结果并触发 TransportTask completion 回调
```

因此它可以理解为：

```text
TransportTask 在 Send 返回后，还需要多久才能确认完成
```

其中包含后端异步处理、CQE 等待、poll 调度和 CQE 处理，不是单次 `Poll()` 函数的纯 CPU 执行时间，也不包含 Send 返回前的 queue/process/Send 同步调用时间。

代码上的完成路径依次是：

1. `SendSubBatchBuffers()` 返回，记录 `sendCompletedAt`；
2. completion worker 在 `CompletionLoop()` 中扫描 INFLIGHT task，并调用 `TransportTaskExecutor::Poll()`；
3. 对每个尚未完成的 sub-batch，读取 `flagBuffer` 并通过 `PollResponseCid()` 判断 CQE/响应是否就绪；未就绪时本轮直接跳过，等待下一轮 poll；
4. 响应就绪后校验 CID，`UnpackResponse()` 解包，转换响应状态并填写每个 entry 的状态；
5. 汇报 connection 成功/失败，释放 send buffer、flag buffer 和 channel inflight 引用，并将该 sub-batch 标记完成；
6. 最后一个 sub-batch 完成后聚合 TransportTask 状态，进入 `TransportTaskManager::NotifyCompletion()`，记录 completion 时延并触发 client 回调。

Dashboard 的 `④ Send-return Completion Analysis` 将总 completion 时延单独展示为 Average 和 P99：

- Average 与 P99 都升高：后端响应等待、CQE 可见性或 completion worker 整体处理能力可能不足；
- 只有 P99 升高：更像偶发慢响应、completion worker 被调度延迟，或某个 sub-batch 成为长尾；
- completion 正常但 TransportTask E2E 高：问题在 Send 返回以前，应回到 transport queue/process/Send 指标。

为定位这条后半段，Dashboard 还展示四个细分 Histogram：

| 指标 | 起点 → 终点 | 用途 |
|---|---|---|
| `kv:kv_fake_backend_task_queue_duration_seconds` | Fake `Send()` 入 worker queue → fake worker 开始处理 | Fake worker 线程不足或积压 |
| `kv:kv_fake_backend_task_process_duration_seconds` | fake worker 开始处理 → `PublishCompletion()` 写入 flag buffer | `fake_backend.latency_us`、模拟后端操作和响应发布耗时 |
| `kv:kv_transport_task_response_wait_duration_seconds` | transport `Send()` 返回 → completion worker 观察到最后一个有效响应 | 后端/Fake 处理、响应可见性和 completion poll 等待的合计 |
| `kv:kv_transport_task_completion_finalize_duration_seconds` | 观察到最后一个有效响应 → TransportTask finalize | 响应解包、entry 状态写入、连接状态处理、资源释放和聚合 |

前两条只在 Fake provider 下产生样本。`response_wait` 不能再严格拆成“Fake 已发布但尚未被 poll 到”的精确时长，因为 Fake provider 与 transport worker 之间没有共享的响应发布时间戳；但结合 Fake queue/process 与 `response_wait`，可以定位慢主要在 Fake 侧还是 completion worker 侧。超时、Send 前失败或没有观察到有效响应的 task 不记录后两条细分时延。

它不是 TransportTask 的完整端到端时延。单个 TransportTask 的完整路径在概念上是：

```text
TransportTask submit → completed
= kv_transport_task_send_duration_seconds
  （submit → Send returned）
+ kv_transport_task_completion_duration_seconds
  （Send returned → completed）
```

`kv_transport_task_e2e_duration_seconds` 从 transport 注册 task 前记录的 `submittedAt` 直接计时到 `TransportTaskManager::NotifyCompletion()`，因此可以准确计算完整 TransportTask 的 P50/P99。Dashboard 仍并排保留前述两个分段边界用于定位慢在哪一段；不要把分段指标的 P50 或 P99 相加，因为分位数相加不等于总时延的分位数。

#### 一个 ClientTask 包含多个 TransportTask

`kv_client_task_e2e_duration_seconds` 覆盖整个 ClientTask，从 API 入口开始，到最后一个 TransportTask completion 回调被聚合、ClientTask Finalize 为止：

```text
ClientTask API 入口
    │
    ├─ enqueue / client queue / client process
    ├─ 按路由拆分 child
    │
    ├─ TransportTask A ─ Send ─ CQE ─ completion 回调 ─┐
    ├─ TransportTask B ─ Send ─ CQE ─ completion 回调 ─┼─ 聚合完成
    └─ TransportTask C ─ Send ─ CQE ─ completion 回调 ─┘
                                                       │
                                                       └─ ClientTask Finalize

    <────────── kv_client_task_e2e_duration_seconds ──────────>
```

如果 A、B、C 完成时间不同，ClientTask 由最后一个完成的 child 决定：

```text
client task end-to-end
= API 入口 → 最后一个 TransportTask 完成并完成 client 聚合
```

三个完成相关指标的区别：

| 指标 | 粒度 | 起点 | 终点 |
|---|---|---|---|
| `kv:kv_transport_task_completion_duration_seconds` | 单个 TransportTask | 该 child 的 `Send()` 返回 | 该 child 的所有 sub-batch 完成，由 completion worker 开始收尾 |
| `kv:kv_transport_task_e2e_duration_seconds` | 单个 TransportTask | 该 child 在 transport task manager 注册前记录的 `submittedAt` | 该 child 的所有 sub-batch 完成，进入 `TransportTaskManager::NotifyCompletion()` |
| `kv:kv_client_task_e2e_duration_seconds` | 整个 ClientTask | 对应调用进入内部 `KvClientImpl::SubmitAsync()` 后记录 `taskStart` 的位置 | 所有 child 完成回调已聚合，ClientTask Finalize |

## 4. Dashboard 统计方式

### 4.1 Average

阶段拆解使用：

```promql
sum(rate(<metric>_sum[$__rate_interval]))
/
sum(rate(<metric>_count[$__rate_interval]))
```

### 4.2 Average、P50 与 P99

边界和端到端指标使用 Histogram bucket：

```promql
histogram_quantile(
  0.99,
  sum by (le) (rate(<metric>_bucket[$__rate_interval]))
)
```

- Average 表示窗口内所有样本的整体均值，容易受少量慢 task 拉高；
- P50 表示典型 task；
- P99 表示长尾；
- P99 高而 P50 正常，通常是偶发排队或慢 child；
- P50、P99 同时升高，通常是整体处理变慢或系统饱和。

`ClientTask: API → all children pre-Send`、`ClientTask: API → all Send returned` 等是从共同起点量到不同终点的**累计边界时延**。Average 适合观察整体成本，P50/P99 适合区分典型请求和长尾；三者应结合阅读。

两条累计边界曲线之差只能粗略判断哪一段变慢，不能解释为某个 child 的纯 `Send()` 时延。一个 ClientTask 有多个 child，最后到达 pre-Send 和最后返回 `Send()` 的不一定是同一个 child。

### 4.3 Task Completion Rate

Dashboard 只保留两条有直接业务意义的 `_count` 增长速率：

| 图例 | 含义 |
|---|---|
| `client tasks completed` | 每秒完成并聚合的 ClientTask 数 |
| `transport tasks completed` | 每秒进入 `TransportTaskManager::NotifyCompletion()` 的 TransportTask 数，包括 Send 前失败或取消的 task |

它们使用 `rate(<metric>_count[$__rate_interval])`，单位是 task/s。Request Rate 表示提交吞吐，`client tasks completed` 表示完成吞吐；两者持续不一致时需要检查积压、超时或失败。`transport tasks completed / client tasks completed` 可粗略观察每个 ClientTask 的 transport fan-out，但会受窗口边界和失败影响。

Average 查询使用 Histogram 的 `_sum / _count`。`Send-return Completion - Average/P99` 展示总 completion Histogram；`Completion Breakdown - Average/P99` 展示上述四条专用细分 Histogram。

## 5. 推荐排障顺序

1. 看 `Task Completion Rate`，确认本轮测试确实有 ClientTask 和 TransportTask 完成。
2. 看 `Request Rate`、`Entry / Key Throughput`，确认实际负载和 batch 规模。
3. 看错误率：operation 曲线是提交失败率，`wait/final result` 才包含 task 最终失败。
4. 看五段 `Pre-Send Stage Breakdown`，判断慢在 client 还是 transport。
5. 看 Pipeline Average/P50/P99：
   - transport pre-Send 正常、transport Send return 升高：重点检查 provider `Send()`；
   - Send return 正常、completion 升高：重点检查后端返回、CQE poll 和 completion worker；
   - client pre-Send 或 client Send 明显高于 transport：重点检查多 child 最慢分支；
   - `ClientTask E2E: API → completed` 升高：结合前述边界确定是哪一段贡献。

## 6. 必要点位总表

下面汇总本文纳入 kv-test Dashboard 的全部必要点位。为避免六种 operation 重复占用表格行，表中的 `<operation>` 表示：

```text
store | load | batch_store | batch_load | query | delete
```

Dashboard 中对应的图例使用连字符，例如 `batch_store` 的图例为 `batch-store`。

| 类别 | 代码指标 | Dashboard 面板 | 图例 / Stat | 展示统计 | 在 Grafana 中的用途 |
|---|---|---|---|---|---|
| 请求量 | `kv:kv_client_<operation>_requests_total`（6 个） | `Total Request Rate`；`Request Rate by Operation` | 顶部总量 Stat；`<operation>` | `rate`，request/s | 确认实际 API 提交负载及各 operation 占比 |
| 数据量 | `kv:kv_client_<operation>_entries_total`（6 个） | `Entry / Key Throughput` | 顶部总量 Stat；`<operation> entries`；`query keys`；`delete keys` | `rate`，entry/s 或 key/s | 确认真实数据吞吐和 batch 放大倍数 |
| 提交错误 | `kv:kv_client_<operation>_errors_total`（6 个） | `Submission and Final-result Error Rate` | `<operation> submit` | errors / requests | 判断错误是否发生在 API 提交阶段 |
| Wait 调用数 | `kv:kv_client_wait_requests_total` | `Final Error Rate`；`Submission and Final-result Error Rate` | 错误区 Stat 和 `wait/final result` 的分母 | 不单独画 count | 每次 `Wait()` 返回计数一次；kv-test bench 通常每个成功提交的 task 调用一次 |
| Wait/最终结果错误 | `kv:kv_client_wait_errors_total` | 同上 | 错误区 `Final Error Rate` Stat；`wait/final result` | errors / Wait calls | 统计 `Wait()` 自身失败或 `TaskResult.status` 失败的调用 |
| Pre-Send 阶段 1 | `kv:kv_client_task_enqueue_duration_seconds` | `Pre-Send Stage Breakdown - Average` | `1 API → client queue` | Average | API 建 task、MR 映射和入队耗时 |
| Pre-Send 阶段 2 | `kv:kv_client_task_queue_duration_seconds` | 同上 | `2 client queue wait` | Average | client worker 排队耗时 |
| Pre-Send 阶段 3 | `kv:kv_client_task_process_duration_seconds` | 同上 | `3 client process` | Average | 路由、拆分 child 和提交 TransportTask 耗时 |
| Pre-Send 阶段 4 | `kv:kv_transport_task_queue_duration_seconds` | 同上 | `4 transport queue wait` | Average | transport executor 排队耗时 |
| Pre-Send 阶段 5 | `kv:kv_transport_task_process_duration_seconds` | 同上 | `5 transport process` | Average | buffer、连接和请求属性等发送准备耗时 |
| Client 边界 | `kv:kv_client_task_pre_send_duration_seconds` | `Required Pipeline Boundaries - Average/P50/P99` | `ClientTask: API → all children pre-Send` | Average、P50、P99 | 判断整个 client task 在发送前是否变慢 |
| Client 边界 | `kv:kv_client_task_send_duration_seconds` | 同上 | `ClientTask: API → all Send returned` | Average、P50、P99 | 判断最慢 child 的 `Send()` 返回边界 |
| Client E2E | `kv:kv_client_task_e2e_duration_seconds` | `Client Task End-to-End P99`；`Required Pipeline Boundaries - Average/P50/P99`；`Task Completion Rate` | 顶部 P99 Stat；`ClientTask E2E: API → completed`；`client tasks completed` | Average、P50、P99、`_count` rate | 最重要的 client task 端到端时延，同时统计完成吞吐 |
| Transport 边界 | `kv:kv_transport_task_pre_send_duration_seconds` | `Required Pipeline Boundaries - Average/P50/P99` | `TransportTask: submit → pre-Send` | Average、P50、P99 | 定位单个 child 的排队和发送准备长尾 |
| Transport 边界 | `kv:kv_transport_task_send_duration_seconds` | 同上 | `TransportTask: submit → Send returned` | Average、P50、P99 | 定位单个 child 的 `Send()` 调用异常 |
| Transport Send 后完成阶段 | `kv:kv_transport_task_completion_duration_seconds` | `Required Pipeline Boundaries - Average/P50/P99`；`Send-return Completion - Average/P99` | `TransportTask: Send returned → completed`；`Send returned → completed` | Average、P50、P99 | 只统计 `Send()` 返回后的后半段；专用图便于定位后端异步完成、CQE/poll 和 completion 长尾；不是 TransportTask E2E |
| Fake worker 队列 | `kv:kv_fake_backend_task_queue_duration_seconds` | `Completion Breakdown - Average/P99` | `Fake worker queue` | Average、P99 | 仅 Fake provider：定位 fake worker 积压 |
| Fake worker 处理 | `kv:kv_fake_backend_task_process_duration_seconds` | 同上 | `Fake backend process → publish` | Average、P99 | 仅 Fake provider：定位 `latency_us`、模拟后端处理与发布响应耗时 |
| 响应观察等待 | `kv:kv_transport_task_response_wait_duration_seconds` | 同上 | `Send returned → final response observed` | Average、P99 | Send 返回后，等待最后一个有效响应被 completion worker 观察到 |
| Completion 本地收尾 | `kv:kv_transport_task_completion_finalize_duration_seconds` | 同上 | `Final response observed → finalized` | Average、P99 | 解包响应、写 entry 状态、资源释放和任务聚合 |
| Transport E2E | `kv:kv_transport_task_e2e_duration_seconds` | `Required Pipeline Boundaries - Average/P50/P99`；`Task Completion Rate` | `TransportTask E2E: submit → completed`；`transport tasks completed` | Average、P50、P99、`_count` rate | TransportTask 从成功入队到完成的完整时延，同时统计所有 TransportTask 的完成吞吐 |

合计为 **36 个指标族**：20 个 Counter 和 16 个 Histogram。Histogram 自带的 `_bucket`、`_sum`、`_count` 属于同一个指标族，不应重复理解成三个独立打点。
