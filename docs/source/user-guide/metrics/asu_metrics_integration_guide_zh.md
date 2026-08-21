# ASU Metrics 接入与埋点设计

## 1. 文档范围

本文针对以下代码目录：

```text
ucm/transport/kv/asu/
```

目标是让 ASU client/transport 的运行指标进入 UCM 现有指标链路，最终可通过 vLLM `/metrics` 读取，并可由 Prometheus、Grafana 或 metrics-view 消费。

本文只给出设计与推荐改动位置，没有直接修改 ASU 业务代码。

本文分析的是 `D:/a_storage/3_debug/unified-cache-management` 当前代码。该工作树中的 Metrics 实现比主开发工作树旧，接入时必须注意静态库实例隔离问题，不能只在 ASU 代码里直接加入 `UpdateStats()`。

## 2. ASU 当前调用架构

ASU 的一次业务请求大致经过四层：

```text
AsuStore
  -> AsuClientImpl::{Query,Load,Store,BatchLoad,BatchStore,Delete}Async
  -> ClientTaskManager
       - 路由到一个或多个 asuId
       - 一个 ClientTask 拆成多个 TransportTask
  -> AsuTransportImpl
       - TransportTask 进入 executeQueue
       - worker 执行 TransportTaskExecutor::Execute
       - 一个 TransportTask 拆成多个 sub-batch
       - provider Send
       - completion worker 轮询 CQE
  -> TransportTaskManager::NotifyCompletion
  -> ClientTaskManager::CompleteTransportTask / Finalize
  -> AsuClientImpl::Wait
  -> AsuStore::Wait 或 Query
```

三个任务粒度不能混为一谈：

| 粒度 | 对象 | 语义 |
| --- | --- | --- |
| Client task | `ClientTask` | 一次 ASU client API 请求，可能包含多个 key/entry，并路由到多个 ASU |
| Transport task | `TransportTask` | Client task 路由到一个 `asuId` 后形成的子任务 |
| Sub-batch | `TransportSubBatchContext` | Transport 根据 server capability 和 batch 配置进一步拆出的实际发送单元 |

因此，一个 client request 可能对应：

```text
1 client task
  -> N transport tasks（N 个 asuId）
     -> M sub-batches（实际 Send/CQE 单元）
```

埋点必须明确统计的是哪一层，否则容易重复计数。例如不能把每个 transport task 都计入 `asu_client_load_requests_total`，否则跨两个 ASU 的一次 client load 会被计算两次。

## 3. 接入前必须解决的 Metrics 实例问题

### 3.1 当前工作树的状态

本文最初分析时，`ucm/shared/metrics/CMakeLists.txt` 使用：

```cmake
add_library(metrics STATIC ...)
pybind11_add_module(ucmmetrics ...)
target_link_libraries(ucmmetrics PRIVATE metrics)
```

当前 API 是：

```cpp
UC::Metrics::SetUp(maxVectorLen);
UC::Metrics::CreateStats(name, type);
UC::Metrics::UpdateStats(name, value);
UC::Metrics::GetAllStatsAndClear();
```

Python `PrometheusStatsLogger` 通过 `ucmmetrics.get_all_stats_and_clear()` 获取指标。

### 3.2 为什么不能简单把 STATIC metrics 链到 ASU

如果执行：

```cmake
target_link_libraries(asu_transport PRIVATE metrics)
```

同时 `ucmmetrics` 也静态链接 `metrics`，那么两个动态对象很可能各自嵌入一份：

```text
libasu_transport.so
  -> Metrics singleton A

ucmmetrics.so
  -> Metrics singleton B
```

ASU 写入 singleton A，而 Python 从 singleton B 执行 `GetAllStatsAndClear()`，最终 `/metrics` 看不到 ASU 数据。

### 3.3 推荐适配方式

先把 collector 改为所有调用方共享的动态库：

```cmake
add_library(metrics SHARED ${UCMMETRICS_CC_SOURCE_FILES})

target_link_libraries(ucmmetrics PRIVATE metrics)
target_link_libraries(asu_transport PRIVATE metrics)
target_link_libraries(asu_client PRIVATE metrics)
```

同时安装 `libmetrics.so`，并保证 `ucmmetrics.so`、`libasu_client.so`、`libasu_transport.so` 最终解析到同一个 `libmetrics.so`：

```cmake
install(TARGETS metrics LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
```

还需验证安装后的 RPATH/加载顺序。建议用以下方式确认：

```bash
ldd <installed-path>/libasu_transport.so | grep metrics
ldd <installed-path>/libasu_client.so | grep metrics
ldd <installed-path>/ucmmetrics.so | grep metrics
```

三者应指向同一份 `libmetrics.so`。

也可以直接将当前主开发工作树中较新的 shared metrics 实现同步到该分支。新版还提供 cached metric id 与更高效的 Histogram 聚合，更适合 ASU 高频路径。

## 4. CMake 适配位置

文件：`ucm/transport/kv/asu/CMakeLists.txt`

### 4.1 `asu_transport`

Transport 层准备加入以下类型的指标，因此需要链接 `metrics`：

```cmake
target_link_libraries(asu_transport
    PUBLIC
        pthread fmt spdlog zlibstatic
    PRIVATE
        asu_ascend_deps
        metrics
)
```

### 4.2 `asu_client`

Client 层负责端到端请求、路由、Wait 和 view refresh 指标，也需要链接：

```cmake
target_link_libraries(asu_client
    PUBLIC asu_transport kv_common pthread
    PRIVATE metrics
)
```

如果第一阶段只做 transport 指标，可以暂时只给 `asu_transport` 链接。但端到端 duration 最适合在 `ClientTaskManager::Finalize` 记录，因此最终建议两边都链接。

### 4.3 测试目标

`asu.test` 通过 `asu_client` 间接获得链接即可。为了直接调用 `SetUp/CreateStats/GetAllStatsAndClear` 写指标断言，也可显式加入：

```cmake
target_link_libraries(asu.test PRIVATE metrics)
```

## 5. 指标命名原则

### 5.1 使用固定名字，不增加高基数标签

当前 UCM Metrics API 不支持 ASU 自定义 label。`model_name`、`worker_id` 是 Python exporter 统一添加的标签。

不要把以下内容拼到 metric name：

- `taskId`
- `clientId`
- key
- IP 地址
- connection handle
- CID
- 动态 `asuId`

否则会产生大量时间序列或大量动态指标名。

操作类型只有固定的 query/load/store/batch_load/batch_store/delete，可以编码在名称中，例如：

```text
asu_client_load_requests_total
asu_client_store_duration_ms
asu_transport_query_sub_batches_total
```

### 5.2 区分 task 和 item

推荐同时提供：

```text
*_requests_total / *_tasks_total  请求或任务数量
*_items_total                     key/entry 数量
*_bytes_total                     load/store 数据量
```

例如一次 `BatchLoadAsync` 带 32 个 entry：

```text
asu_client_batch_load_requests_total += 1
asu_client_batch_load_items_total    += 32
asu_client_batch_load_bytes_total    += sum(entry.buffer.region.size)
```

### 5.3 `errors_total` 的定义

建议只在“该层最终判定失败”的位置记录该层 error：

- Client error：一个 ClientTask 最终失败或部分失败。
- Transport error：一个 TransportTask 最终失败或部分失败。
- Send error：provider `Send()` 返回失败。
- CQE error：CQE 解码成功，但设备返回错误状态。

不同层的 error 可以同时增加，因为它们回答不同问题；同一层不要在提交失败、回调和 Wait 中对同一个失败重复增加。

## 6. 第一阶段推荐指标（P0）

第一阶段应优先保证端到端请求、吞吐、延迟和关键错误可用，不要一次加入过多细节。

### 6.1 Client 层 Counter

下表中的 `{op}` 是固定展开的 `query`、`load`、`store`、`batch_load`、`batch_store`、`delete`，不是 Prometheus label。

| 指标 | 含义 | 更新位置 |
| --- | --- | --- |
| `asu_client_{op}_requests_total` | 接受的 client 请求数 | 两个 `AsuClientImpl::SubmitAsync` 成功进入 client queue 后 |
| `asu_client_{op}_items_total` | 请求携带的 key/entry 数 | 同上 |
| `asu_client_{op}_bytes_total` | load/store 请求字节数 | entry 版 `SubmitAsync`；只对四种 entry op 更新 |
| `asu_client_{op}_completed_total` | 最终完成的 client task 数 | `ClientTaskManager::Finalize` 和 `CompleteWithError` 的统一完成路径 |
| `asu_client_{op}_errors_total` | 最终失败/部分失败的 client task 数 | 同上，仅最终状态非 OK 时加 1 |
| `asu_client_wait_timeouts_total` | 调用者 Wait 超时次数 | `ClientTaskManager::WaitContext` 的 `!done` 分支 |
| `asu_client_submit_errors_total` | 请求未能进入 client queue | `AsuClientImpl::SubmitAsync` 的前置检查、Submit/queue 失败出口 |

建议不要在 `LoadAsync()`、`BatchLoadAsync()` 等薄封装函数里分别复制大量埋点。两个 `SubmitAsync` 是所有 entry/key 请求的统一入口，更不容易漏掉操作。

### 6.2 Client 层 Histogram

| 指标 | 起点 | 终点 |
| --- | --- | --- |
| `asu_client_{op}_duration_ms` | 请求成功进入 client queue | ClientTask 最终完成 |
| `asu_client_queue_wait_duration_ms` | 进入 `taskQueue_` | `WorkerLoop` 取出 task |
| `asu_client_route_duration_ms` | `BuildTransportTasks` 开始 | 路由和 TransportTask 构建完成 |
| `asu_client_transport_tasks` | ClientTask 构建完成 | 记录拆出的 transport task 数量 |

端到端 duration 不能在 `AsuClientImpl::Wait` 中记录，因为：

- 用户可能只调用 `Check`，不调用 `Wait`；
- 用户可能很晚才调用 `Wait`，会把调用者等待前的空闲时间混入；
- `Wait` 超时并不代表底层 task 已最终失败。

正确终点是 task 真正进入 COMPLETED 的地方。

### 6.3 Transport 层 Counter

| 指标 | 含义 | 更新位置 |
| --- | --- | --- |
| `asu_transport_tasks_total` | 成功进入 transport execute queue 的任务数 | `AsuTransportImpl::SubmitTask` 的 `TryPush` 成功后 |
| `asu_transport_queue_full_total` | execute queue 满导致拒绝 | `TryPush` 失败分支 |
| `asu_transport_sub_batches_total` | 实际构建的 sub-batch 数 | `PrepareTaskSubBatches` 成功后/Execute 中 |
| `asu_transport_send_errors_total` | provider Send 失败的 sub-batch 数 | `SendSubBatchBuffers` |
| `asu_transport_no_channel_total` | 没有可用 connection channel | `AssignSubBatchConnections` |
| `asu_transport_timeouts_total` | TransportTask deadline 超时数 | `TransportTaskExecutor::Poll` deadline 分支，每 task 加 1 |
| `asu_transport_cqe_errors_total` | CQE 返回设备错误的 sub-batch 数 | `Poll` 解包并得到 response status 后 |
| `asu_transport_response_decode_errors_total` | CQE header/full buffer copy 或 unpack 失败 | `Poll` 对应失败分支 |
| `asu_transport_tasks_completed_total` | 最终完成的 TransportTask 数 | `TransportTaskManager::NotifyCompletion` |
| `asu_transport_task_errors_total` | 最终失败的 TransportTask 数 | 同上，`finalStatus` 非 OK 时 |

### 6.4 Transport 层 Histogram/Gauge

| 指标 | 类型 | 含义 |
| --- | --- | --- |
| `asu_transport_queue_wait_duration_ms` | Histogram | transport task 入 executeQueue 到 worker 开始 Execute |
| `asu_transport_task_duration_ms` | Histogram | transport task 成功入队到最终完成 |
| `asu_transport_send_duration_ms` | Histogram | 单次批量调用 provider `Send()` 的同步耗时；不代表远端 I/O 完成时间 |
| `asu_transport_sub_batch_duration_ms` | Histogram | sub-batch 发送完成到 CQE 完成/失败 |
| `asu_transport_inflight_tasks` | Gauge | 当前 transport task 数；提交成功 +1，最终完成 -1 |
| `asu_transport_inflight_sub_batches` | Gauge | 已发送但尚未完成的 sub-batch 数 |

当前 Gauge 是“最后写入值覆盖”。如果多个 Transport 实例同时写同名 Gauge，最后写入者并不等于所有 transport 的总和。因此 P0 可以先不做 per-transport Gauge，或在更高的 owner 层维护一个进程级原子总数再写 Gauge。

## 7. 第二阶段诊断指标（P1）

P1 用于区分 buffer、connection、provider、协议和 view refresh 瓶颈。

| 指标 | 类型 | 推荐位置 |
| --- | --- | --- |
| `asu_transport_send_buffer_exhausted_total` | Counter | `BufferManager::Allocate` 的 `IndexPool::npos`，只针对 send buffer |
| `asu_transport_flag_buffer_exhausted_total` | Counter | 同上，只针对 flag buffer |
| `asu_transport_buffer_release_errors_total` | Counter | `ReleaseSubBatchResources` 中 `Free` 失败 |
| `asu_transport_connection_failures_total` | Counter | `ConnectionManager::ReportFailure` |
| `asu_transport_channel_drains_total` | Counter | `MarkForDrain()` 成功之后 |
| `asu_transport_recovery_attempts_total` | Counter | `ConnectionManager::RecoverLoop` 每次 CreateConnection 前 |
| `asu_transport_recovery_success_total` | Counter | 恢复成功并 AddChannel 后 |
| `asu_transport_recovery_errors_total` | Counter | 恢复 CreateConnection 失败 |
| `asu_client_view_refresh_requests_total` | Counter | `RequestBackgroundRefresh` 真正启动新 refresh thread 时 |
| `asu_client_view_refresh_success_total` | Counter | `RefreshView` 发布新 snapshot 成功 |
| `asu_client_view_refresh_errors_total` | Counter | `RefreshView` 失败 |
| `asu_client_view_refresh_duration_ms` | Histogram | `RefreshView` 完整耗时 |
| `asu_client_register_memory_duration_ms` | Histogram | `RegisterRegionsOnce` |
| `asu_client_register_memory_errors_total` | Counter | Register/bind/token/rollback 失败 |

不要在 `ConnectionManager::SelectConnection()` 每次成功时记录 Histogram；这是极高频路径，且对核心诊断帮助有限。优先记录“选不到 channel”和 inflight/queue 指标。

## 8. 需要增加的时间字段

为了正确测量异步生命周期，推荐给 task context 增加 `steady_clock` 时间戳。

文件：`ucm/transport/kv/asu/common/task_context.h`

### 8.1 ClientTask

```cpp
struct ClientTask {
    // 原字段...
    std::chrono::steady_clock::time_point acceptedAt{};
    std::chrono::steady_clock::time_point processingAt{};
    std::atomic<bool> metricsRecorded{false};
};
```

- `acceptedAt`：成功加入 `taskQueue_` 时设置。
- `processingAt`：`WorkerLoop` 取出、进入 `ClientTaskManager::Process` 前设置。
- `metricsRecorded`：保证 `Finalize`、`CompleteWithError` 等竞争/异常路径只记录一次最终指标。

### 8.2 TransportTask

```cpp
struct TransportTask {
    // 原字段...
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point executingAt{};
    std::atomic<bool> metricsRecorded{false};
};
```

- `enqueuedAt`：`executeQueue_.TryPush()` 成功之前/之后紧邻设置。
- `executingAt`：`TransportTaskExecutor::Execute` 成功把 PENDING CAS 为 INFLIGHT 后设置。
- 最终 duration：在 `TransportTaskManager::NotifyCompletion` 首次通知时记录。

### 8.3 TransportSubBatchContext

文件：`trans/src/transport_task_manager.h`

```cpp
struct TransportSubBatchContext {
    // 原字段...
    std::chrono::steady_clock::time_point sentAt{};
    bool metricsRecorded{false};
};
```

- `sentAt`：对应 `Send` status 成功时设置。
- 结束：统一经过 `CompleteSubBatch` 时记录。
- 对 pre-send failure 没有 sentAt，不记录远端 sub-batch duration，只记录具体错误 counter。

## 9. 具体代码埋点位置

### 9.1 `AsuClientImpl::SubmitAsync(entries)`

文件：`client/src/asu_client_impl.cpp`，当前约 333 行。

建议：

1. 计算 `itemCount = entries.size()`。
2. 计算 `bytes = sum(entry.buffer.region.size)`，并处理 double 转换。
3. 在任务成功放入 `taskQueue_` 后设置 `acceptedAt`。
4. 增加对应 op 的 requests/items/bytes Counter。
5. 所有提前返回失败路径只增加 `asu_client_submit_errors_total`，不要增加 completed/error，因为 task 尚未被接受。

入口函数返回 OK 只表示“已接受异步任务”，不表示实际 I/O 成功，因此不能在这里增加 success counter。

### 9.2 `AsuClientImpl::SubmitAsync(keys)`

文件同上，当前约 388 行。

与 entries 版相同，但只记录 requests/items，不记录 bytes。

### 9.3 `AsuClientImpl::WorkerLoop`

文件同上，当前约 432 行。

在从 `taskQueue_` pop 后：

```text
processingAt = now
queue_wait_ms = processingAt - acceptedAt
```

记录 `asu_client_queue_wait_duration_ms`。这个点可以直接看 client worker 是否堵塞。

### 9.4 `ClientTaskManager::BuildTransportTasks`

文件：`client/src/client_task_manager.cpp`，当前约 245 行。

围绕 RouteKeys + TransportTask 构建记录：

- `asu_client_route_duration_ms`
- `asu_client_transport_tasks` Histogram，值为 `routes.size()`

这里适合记录路由放大程度，但不要再次记录 client items/bytes。

### 9.5 `ClientTaskManager::Finalize`

文件同上，当前约 210 行。

这是正常/部分失败 ClientTask 的最佳最终埋点：

- completed +1
- `finalStatus` 非 OK 时 errors +1
- duration = now - acceptedAt
- Query 可记录 `asu_client_query_hit_items_total`，值为 `count(exists != 0)`
- Query 可记录 `asu_client_query_prefix_hit_items_total`，值为 `prefixHitKeys`

用 `metricsRecorded.exchange(true)` 保护，避免重复记录。

### 9.6 `ClientTaskManager::CompleteWithError`

文件同上，当前约 137 行。

BuildTransportTasks 失败会走这里，不会进入普通 Finalize。因此它需要调用同一个 `RecordClientTaskCompletion(task)` helper，不能只在 Finalize 埋点。

推荐抽出统一 helper，而不是在两个函数中复制指标逻辑。

### 9.7 `ClientTaskManager::WaitContext`

文件同上，当前约 335 行。

只在 `!done` 分支记录：

```text
asu_client_wait_timeouts_total += 1
```

不要在此增加 client task error：Wait timeout 是调用者等待超时，task 后续仍可能成功完成。

### 9.8 `AsuTransportImpl::SubmitTask`

文件：`trans/src/asu_transport_impl.cpp`，当前约 260 行。

- `TryPush` 失败：`asu_transport_queue_full_total += 1`
- `TryPush` 成功：设置 `enqueuedAt`，`asu_transport_tasks_total += 1`
- 若维护进程级 inflight：成功后 +1

这里返回 OK 仍只表示 transport 接受任务，不代表 I/O 成功。

### 9.9 `TransportTaskExecutor::Execute`

文件：`trans/src/transport_task_executor.cpp`，当前约 188 行。

CAS 成功后设置 `executingAt`，记录 transport queue wait。

`PrepareTaskSubBatches` 成功后记录 sub-batch 数量。分别给下列失败增加诊断 Counter：

- Prepare failure
- Assign connection failure
- pre-send abort

不要在 Execute 返回 true 和后续 `NotifyCompletion` 各记一次 completion；最终完成统一交给 TaskManager。

### 9.10 `TransportTaskExecutor::SendSubBatchBuffers`

文件：`trans/src/asu_submit_flow.cpp`，当前约 131 行。

围绕一次 `transProvider_->Send(...)` 记录同步调用耗时：

```text
asu_transport_send_duration_ms
```

这个指标只能叫 send/submit duration，不能叫 load/store duration，因为 Send 返回时远端 I/O 还未完成。

- status count 不匹配：按受影响 sub-batch 数增加 send errors。
- 单项 status 失败：每个失败 sub-batch 加 1。
- 单项成功：设置对应 `sentAt`。

### 9.11 `TransportTaskExecutor::Poll`

文件：`trans/src/transport_task_executor.cpp`，当前约 238 行。

重点位置：

- deadline 分支：每个 TransportTask 增加一次 `asu_transport_timeouts_total`。
- device-to-host header copy 失败：response decode/copy error。
- full flag buffer copy 失败：response decode/copy error。
- `UnpackResponse` 失败：response decode error。
- CQE status 非 OK：CQE error；query 的 `ASU_CQE_CHECK_RESULT_BUFFER` 是业务结果载体，不应简单算作系统错误。
- 每个 sub-batch 进入 `CompleteSubBatch` 时记录一次最终 duration。

### 9.12 `TransportTaskManager::NotifyCompletion`

文件：`trans/src/transport_task_manager.cpp`，当前约 56 行。

在真正回调并 Remove task 前统一记录：

- transport completed +1
- transport error +1（非 OK）
- task duration
- 进程级 inflight -1

利用 `completionNotified` 或独立 `metricsRecorded` 保证一次性。

### 9.13 `BufferManager::Allocate`

文件：`trans/src/buffer_manager.cpp`，当前约 197 行。

`IndexPool::npos` 是重要饱和信号。但 `BufferManager` 同时服务 send/flag buffer，当前只有 `name_` 区分。因为 Metrics 不支持 label，建议：

- 在 `TransportTaskExecutor` 调用 Allocate 的具体位置记录 send/flag exhaustion；或者
- 给 BufferManager 增加固定 enum kind，而不是把任意 `name_` 拼入指标名。

### 9.14 `ConnectionManager`

文件：`trans/src/connection_manager.cpp`。

推荐埋点：

- `SelectConnection` 返回 null：no-channel Counter，但如果上层 `AssignSubBatchConnections` 已记录，这里不要重复。
- `ReportFailure`：connection failure Counter。
- `MarkForDrain` 成功：channel drain Counter。
- `RecoverLoop` CreateConnection 前/成功/失败：recovery 三组 Counter。

## 10. 建议增加统一 helper

当前 API 每次根据字符串查 metric 类型，ASU 高频路径如果到处写字符串，可读性和维护性都较差。建议至少增加 ASU 内部 helper：

```cpp
namespace UC::ASU::Metrics {

double DurationMs(std::chrono::steady_clock::time_point begin);
std::size_t ItemCount(const ClientTask& task);
std::uint64_t ByteCount(const std::vector<KVBuffer>& entries);

void RecordClientAccepted(AsuOpType op, std::size_t items, std::uint64_t bytes);
void RecordClientCompleted(const ClientTask& task);
void RecordTransportCompleted(const TransportTask& task);

}  // namespace UC::ASU::Metrics
```

可放在：

```text
ucm/transport/kv/asu/common/asu_metrics.h
ucm/transport/kv/asu/common/asu_metrics.cpp
```

helper 内部用固定 switch 将 `AsuOpType` 映射到静态 metric name，避免运行时拼接字符串：

```cpp
const char* ClientDurationMetric(AsuOpType op)
{
    switch (op) {
        case AsuOpType::QUERY: return "asu_client_query_duration_ms";
        case AsuOpType::LOAD: return "asu_client_load_duration_ms";
        // ...
    }
}
```

如果同步新版 Metrics API，应优先使用 `NAME_TO_METRIC_ID(...)`/`CachedMetric`，避免每次字符串查表。

## 11. metrics_configs.yaml 适配

所有新增指标必须加入：

```text
examples/metrics/metrics_configs.yaml
```

否则当前 `PrometheusStatsLogger` 不会执行 `create_stats()`，C++ `UpdateStats()` 会因指标未注册而失效。

P0 示例：

```yaml
counter:
  - name: "asu_client_batch_load_requests_total"
    documentation: "Number of ASU client batch-load requests accepted"
  - name: "asu_client_batch_load_items_total"
    documentation: "Number of entries accepted by ASU client batch-load requests"
  - name: "asu_client_batch_load_bytes_total"
    documentation: "Bytes accepted by ASU client batch-load requests"
  - name: "asu_client_batch_load_completed_total"
    documentation: "Number of completed ASU client batch-load tasks"
  - name: "asu_client_batch_load_errors_total"
    documentation: "Number of failed or partially failed ASU client batch-load tasks"
  - name: "asu_client_wait_timeouts_total"
    documentation: "Number of ASU client Wait calls that timed out"
  - name: "asu_transport_queue_full_total"
    documentation: "Number of ASU transport tasks rejected because the execute queue was full"
  - name: "asu_transport_send_errors_total"
    documentation: "Number of ASU transport sub-batches rejected by provider Send"
  - name: "asu_transport_timeouts_total"
    documentation: "Number of ASU transport tasks that reached their execution deadline"

histogram:
  - name: "asu_client_batch_load_duration_ms"
    documentation: "End-to-end ASU client batch-load task duration in milliseconds"
    buckets: [0.1, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000]
  - name: "asu_client_queue_wait_duration_ms"
    documentation: "Time an ASU client task waits in the client worker queue"
    buckets: [0.01, 0.05, 0.1, 0.5, 1, 2, 5, 10, 20, 50, 100]
  - name: "asu_transport_queue_wait_duration_ms"
    documentation: "Time an ASU transport task waits in the execute queue"
    buckets: [0.01, 0.05, 0.1, 0.5, 1, 2, 5, 10, 20, 50, 100]
  - name: "asu_transport_send_duration_ms"
    documentation: "Synchronous provider Send call duration; excludes remote completion"
    buckets: [0.01, 0.05, 0.1, 0.5, 1, 2, 5, 10, 20, 50]
```

注意：当前旧版 Metrics collector 的 Histogram 在 C++ 内保存原始样本 vector，YAML buckets 只在 Python Prometheus Histogram 注册时生效。高频 sub-batch Histogram 可能带来较大内存和 drain 成本，这也是建议同步新版 bucket-count collector 的另一个原因。

## 12. 与 AsuStore 指标的边界

`ucm/store/asu/cc/asu_store.cc` 是 StoreV1 适配层。它还包含：

- BlockId 到 ASU key 的转换；
- KV tensor/shard 到 `KVBuffer` 的构建；
- Dump prerequisite event 等待；
- UCM Store task 到 ASU client task 的映射。

如果需要回答“UCM 的 AsuStore Lookup/Load/Dump 整体多慢”，应在 AsuStore 层增加：

```text
asu_store_lookup_duration_ms
asu_store_load_duration_ms
asu_store_dump_duration_ms
asu_store_dump_prereq_wait_duration_ms
asu_store_load_bytes_total
asu_store_dump_bytes_total
```

这些不能用 client duration 完全替代，因为 Store 层还包含 key/buffer 构建与 prerequisite wait。

建议层次定义：

```text
asu_store_*       UCM StoreV1 端到端视角
asu_client_*      client API、路由、多 ASU 聚合视角
asu_transport_*   queue、provider、connection、CQE 视角
```

三层可以同时存在，但名称和文档必须明确，避免用户把它们当作同一段耗时。

## 13. 测试方案

### 13.1 现有 `asu_client_e2e_metrics_test.cpp` 的性质

该测试中的 `MetricsRecorder` 是测试专用性能统计器，只在测试进程中保存 samples 并打印报告。它不会调用 `UC::Metrics::UpdateStats()`，也不会进入 `/metrics`。

可以保留它用于 correctness/performance workload，同时新增正式 collector 断言。

### 13.2 单元测试初始化

测试开始时：

```cpp
UC::Metrics::SetUp(10000);
UC::Metrics::CreateStats("asu_transport_queue_full_total", "counter");
UC::Metrics::CreateStats("asu_transport_task_duration_ms", "histogram");
```

驱动业务后：

```cpp
auto [counters, gauges, histograms] = UC::Metrics::GetAllStatsAndClear();
EXPECT_EQ(counters["asu_transport_queue_full_total"], expected);
EXPECT_EQ(histograms["asu_transport_task_duration_ms"].size(), completedTasks);
```

### 13.3 必测场景

| 场景 | 应验证的指标 |
| --- | --- |
| BatchStore + BatchLoad 成功 | requests/items/bytes/completed/duration |
| 一个 client task 路由多个 ASU | client request 仍为 1，transport task 为 N |
| batch 被拆为多个 sub-batch | sub-batches_total 等于实际拆分数 |
| executeQueue 满 | queue_full_total |
| 无可用 channel | no_channel_total |
| provider Send 单项失败 | send_errors_total，最终 transport/client error |
| CQE internal error | cqe_errors_total、connection failure/drain |
| Transport deadline | transport_timeouts_total，只按 task 计一次 |
| Client Wait 超时但任务后来成功 | wait_timeouts_total + 最终 completed，不应误加最终 error |
| view refresh 成功/失败 | refresh request/success/error/duration |
| 多线程并发提交和 drain | 不丢 Counter，不重复 completion |

### 13.4 端到端验证

1. 在 `metrics_configs.yaml` 注册 ASU 指标。
2. vLLM UCM 配置设置 `metrics_config_path`。
3. 启动包含 AsuStore 的 vLLM。
4. 发请求触发 ASU load/store/query。
5. 等待 Python `log_interval`（当前默认 5 秒）让 `PrometheusStatsLogger` drain C++ collector。
6. 查询：

```bash
curl -s http://127.0.0.1:8000/metrics | grep '^ucm:asu_'
```

如果 C++ 单测能读到 ASU 指标、但 `/metrics` 没有，首先检查：

- `libasu_transport.so` 与 `ucmmetrics.so` 是否使用同一个 `libmetrics.so`；
- YAML 是否注册了完全一致的名称；
- `PrometheusStatsLogger` 是否启动；
- `log_interval` drain 是否已经执行；
- ASU 执行所在进程是否就是 stats logger 所在进程。

## 14. 推荐实施顺序

### 阶段 0：统一 collector

1. 将 `metrics` 从 STATIC 调整为 SHARED，或同步新版 metrics 模块。本分支当前实现已经完成该调整，输出 `libucm_metrics.so`。
2. `asu_client`、`asu_transport`、`ucmmetrics` 链接同一 collector。
3. 写一个最小测试：ASU DSO 调用 `UpdateStats`，Python `ucmmetrics.get_all_stats_and_clear()` 能读取到。

这一步通过前，不要批量加埋点。

### 阶段 1：Client P0

1. 增加 ClientTask 时间戳与一次性记录保护。
2. 接入 accepted requests/items/bytes。
3. 在统一 completion helper 中记录 completed/errors/duration。
4. 加 wait timeout 和 queue wait。

### 阶段 2：Transport P0

1. 增加 TransportTask/SubBatch 时间戳。
2. 接入 queue full/no channel/send error/timeout/CQE error。
3. 接入 task/sub-batch duration。

### 阶段 3：连接与资源诊断

增加 buffer exhaustion、channel drain、recovery、view refresh 和 memory registration 指标。

### 阶段 4：展示

1. 在 `/metrics` 验证原始序列。
2. 为 ASU 增加 Grafana dashboard 或扩展 pipeline dashboard。
3. 为 metrics-view 增加 ASU preset（如果需要终端汇总）。

## 15. 最终推荐

最稳妥的最小落地方案是：

```text
先共享 Metrics collector
  -> ClientTask 记录端到端 requests/items/bytes/duration/error
  -> TransportTask 记录 queue/send/CQE/timeout
  -> SubBatch 只记录实际发送单元与完成耗时
  -> YAML 注册全部名字
  -> C++ 单测验证数值
  -> Python/vLLM 端到端验证 /metrics
```

不要从 `AsuClientImpl::Wait` 单点测量所有耗时，也不要只在 provider `Send()` 周围把耗时命名为 load/store latency；ASU 是异步多层拆分架构，必须将 client 端到端、transport queue、send submit 和 CQE completion 分开记录。
