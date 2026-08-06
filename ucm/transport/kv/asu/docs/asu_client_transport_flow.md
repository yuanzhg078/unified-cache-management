# ASU Client → Transport 请求流程分析

本文档梳理一个 `Query` 和一个 `Load/Store` 命令从 `AsuClient` 到 `AsuTransport` 的完整处理流程，重点说明 Transport 侧的处理细节。

## 1. 核心组件与职责

| 层级            | 组件                             | 文件                                                                        | 职责                                                                    |
| ------------- | ------------------------------ | ------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| Client 接口     | `AsuClient`                    | [asu\_client.h](../client/include/asu_client/asu_client.h)                | 对外异步 API：`QueryAsync/LoadAsync/StoreAsync/DeleteAsync` + `Check/Wait` |
| Client 实现     | `AsuClientImpl`                | [asu\_client\_impl.cpp](../client/src/asu_client_impl.cpp)                | 路由视图管理、提交 `ClientTask` 到 worker 队列、结果聚合                               |
| Client 任务管理   | `ClientTaskManager`            | [client\_task\_manager.cpp](../client/src/client_task_manager.cpp)        | 拆分 `ClientTask` 为多个 `TransportTask`、分发、回调聚合、等待                        |
| Transport 接口  | `AsuTransport`                 | [asu\_transport.h](../trans/include/asu_transport/asu_transport.h)        | 单个 ASU 的数据面：`Submit/Cancel/RegisterRegions`                           |
| Transport 实现  | `AsuTransportImpl`             | [asu\_transport\_impl.cpp](../trans/src/asu_transport_impl.cpp)           | 任务入队、worker/completion 双线程、资源管理                                       |
| Transport 执行器 | `TransportTaskExecutor`        | [transport\_task\_executor.cpp](../trans/src/transport_task_executor.cpp) | 子批次拆分、SQE 打包、连接分配、下发、轮询响应                                             |
| IO 调度         | `IoScheduler`                  | [io\_scheduler.cpp](../trans/src/io_scheduler.cpp)                        | 按操作类型把 key/entry 批切分为多个子批次                                            |
| KV 协议         | `ProtocolManager`/`KvProtocol` | [kv\_protocol.h](../trans/src/kv_protocol.h)                              | SQE 打包 / CQE 解包                                                       |
| SQE 构建        | `sqe_request.cpp`              | [sqe\_request.cpp](../trans/src/sqe_request.cpp)                          | 构造 `KvExist/KvBatchStore/KvBatchRetrieve/...` 请求并 pack                |
| 连接管理          | `ConnectionManager`            | [connection\_manager.h](../trans/src/connection_manager.h)                | 多 QP 连接选择（轮询/最少负载）、故障上报与恢复                                            |
| Buffer 管理     | `BufferManager`                | [buffer\_manager.h](../trans/src/buffer_manager.h)                        | send buffer / flag buffer 的 slot 分配与回收                                |
| Provider      | `TransProvider`                | [trans\_provider.h](../trans/src/trans_provider.h)                        | 底层硬件抽象：建链、`Send`、内存注册                                                 |

## 2. 关键数据结构

```
ClientTask (client 层聚合任务)
  ├── opType: ClientOpType (QUERY/LOAD/STORE/DELETE)
  ├── viewSnapshot: 路由 + transports 快照
  ├── keys / entries: 原始输入
  ├── transportTasks: vector<TransportTaskPtr>   # 按 ASU 拆分
  └── remainingTransportTasks: 原子计数

TransportTask (单个 ASU 的传输任务)
  ├── opType: TransportOpType (QUERY/BATCH_LOAD/BATCH_STORE/DELETE)
  ├── asuId, transport(weak_ptr)
  ├── keys / entries / originalIndices
  ├── subBatchContexts: shared_ptr<vector<TransportSubBatchContext>>  # 按 IO 数量拆分
  └── remainingSubBatchCount

TransportSubBatchContext (单次硬件下发的子批次)
  ├── cid: 16bit 请求标识
  ├── channel: ConnectionChannel
  ├── sendSge: 发送缓冲(SQE 已 pack)
  ├── flagBuffer: 响应缓冲(CQE)
  └── entryStatus: 逐条目状态
```

> 注意 ClientOpType→TransportOpType 的映射（见 [client\_task\_manager.cpp#L322](../client/src/client_task_manager.cpp#L322-L325)）：
>
> - `QUERY → QUERY`（KvOpcode::Exist）
> - `LOAD → BATCH_LOAD`（KvOpcode::BatchRetrieve）
> - `STORE → BATCH_STORE`（KvOpcode::BatchStore）
> - `DELETE → DELETE`（KvOpcode::Delete）

## 3. Query 流程时序图

```mermaid
sequenceDiagram
    autonumber
    participant User
    participant C as AsuClientImpl
    participant CQ as taskQueue client
    participant CTM as ClientTaskManager
    participant R as Router
    participant T as AsuTransportImpl
    participant EQ as executeQueue
    participant TE as TransportTaskExecutor
    participant IOS as IoScheduler
    participant PM as ProtocolManager
    participant CM as ConnectionManager
    participant BM as BufferManager
    participant TP as TransProvider

    User->>C: QueryAsync(keys, options, taskId)
    C->>C: SubmitAsync QUERY keys timeoutMs
    C->>C: GetSnapshot 取路由快照
    C->>CTM: Submit ClientTask QUERY keys taskId
    C->>CQ: push task notify_one
    C-->>User: Status OK taskId

    Note over C: WorkerLoop 线程
    CQ-->>C: pop task
    C->>CTM: Process task
    CTM->>CTM: state = INFLIGHT
    CTM->>R: RouteKeys keys
    R-->>CTM: routes asuId to indices
    CTM->>CTM: BuildTransportTasks<br/>每个 ASU 一个 TransportTask QUERY
    CTM->>CTM: DispatchTask 遍历 transportTasks
    CTM->>T: transport Submit transportTask<br/>设 onComplete 回调

    T->>T: Submit 设 entryStatus deadline
    T->>T: SubmitTask 锁 producerMu
    T->>T: taskManager Submit task
    T->>EQ: TryPush task
    T-->>CTM: OK

    Note over T: WorkerLoop 线程消费 executeQueue
    EQ-->>T: pop task
    T->>TE: Execute task
    TE->>TE: CAS PENDING 到 INFLIGHT
    TE->>TE: SubmitTaskRequests<br/>IsKeyBatchOp = true
    TE->>IOS: SplitForAsu keys QUERY<br/>每批最多 queryIoNum 256
    IOS-->>TE: subBatches
    loop 每个 subBatch
        TE->>TE: SubmitKeySubBatchRequest
        TE->>TE: AllocateRequestCid
        TE->>BM: Allocate flagBuffer<br/>Exist 16 加 (n+7)/8 字节
        TE->>TE: BuildExistRequest keys attrs cid flagBuffer
        TE->>PM: GetPackedSize Exist
        TE->>BM: Allocate sendSge packedSize
        TE->>PM: PackRequest sendSge Exist req
    end
    TE->>CM: AssignSubBatchConnections<br/>SelectConnection 轮询
    TE->>TE: BuildSubBatchSendBuffers<br/>组装 SendIoBatch conn send flag len
    TE->>TP: Send ioBatches kernelCount quietCount
    TP-->>TE: sendStatuses
    TE->>TE: 存 subBatchContexts 到 task<br/>InitializeRemainingSubBatchCount<br/>TryFinalizeFromSubBatches
    TE-->>T: done? false 等待 CQE
```

### 3.1 Query 响应轮询与回调

```mermaid
sequenceDiagram
    autonumber
    participant User as 调用方
    participant CL as CompletionLoop 线程
    participant TE as TransportTaskExecutor
    participant PM as ProtocolManager
    participant CM as ConnectionManager
    participant BM as BufferManager
    participant TTM as TransportTaskManager
    participant TT as TransportTask
    participant CB as onComplete 回调
    participant CTM as ClientTaskManager

    Note over CL: 每 1ms 遍历所有 INFLIGHT 任务
    loop 每个未完成 subBatch
        CL->>TE: Poll(task)
        TE->>TE: 读 flagBuffer (device→host 拷贝头 16B)
        TE->>PM: PollResponseCid(flagData, completedCid)
        alt cid 未完成
            PM-->>TE: not ready
            TE-->>CL: false
        else cid 匹配
            TE->>TE: 拷贝完整 flagBuffer(device→host)
            TE->>PM: UnpackResponse(flagData, Exist, batchNum, KvResponse)
            PM-->>TE: KvResponse(status, result_buffer)
            TE->>TE: KvResponseStatusToSubBatchStatus<br/>FillEntryStatusFromCqeResult
            alt 内部错误/超时
                TE->>CM: ReportFailure(channel)
            else 成功
                TE->>CM: ReportSuccess(channel)
            end
            TE->>TE: CompleteSubBatch<br/>释放 send/flag slot<br/>ReleaseInflight<br/>--remainingSubBatchCount
            TE->>TE: TryFinalizeFromSubBatches
            opt 全部完成
                TE->>TE: state=COMPLETED
                TE-->>CL: done=true
            end
        end
    end

    CL->>TTM: NotifyCompletion(task)
    TTM->>TTM: BuildResult<br/>(QUERY: BuildQueryResultFromEntryStatus)
    TTM->>TT: NotifyCompletion(result)
    TT->>TT: completionNotified.exchange(true)
    opt 首次通知且存在 onComplete
        TT->>CB: onComplete(result)
        CB->>CTM: CompleteTransportTask(clientTask, idx, result)
        CTM->>CTM: 散列 queryResult.exists 到 originalIndices<br/>累加 prefixHitKeys<br/>回填 entryStatus
        CTM->>CTM: --remainingTransportTasks
        opt 归零
            CTM->>CTM: Finalize<br/>state=COMPLETED, cv.notify_all
        end
    end
    TTM->>TTM: Remove(transportTaskId)

    Note over User: 调用方 Wait
    User->>CTM: Wait(taskId, timeout, result)
    CTM->>CTM: cv.wait_for 直到 Done
    CTM->>CTM: BuildResult + Remove(taskId)
    CTM-->>User: TaskResult status entryStatus queryResult
```

### 3.2 Transport 内部 Query 处理时序图

聚焦 Transport 层内部：从 `Submit` 入队到 `Execute` 下发再到 `Poll` 完成回调，不涉及 Client/Router。

```mermaid
sequenceDiagram
    autonumber
    participant T as AsuTransportImpl
    participant TM as TransportTaskManager
    participant EQ as executeQueue SPSC
    participant TE as TransportTaskExecutor
    participant IOS as IoScheduler
    participant BM as BufferManager
    participant PM as ProtocolManager
    participant CM as ConnectionManager
    participant TP as TransProvider

    Note over T: ① 入队
    T->>T: Submit 设 entryStatus deadline
    T->>TM: Submit task 分配 taskId
    T->>EQ: TryPush task

    Note over T: ② WorkerLoop 消费
    EQ-->>T: pop task
    T->>TE: Execute task
    TE->>TE: CAS PENDING 到 INFLIGHT
    TE->>TE: SubmitTaskRequests IsKeyBatchOp=true
    TE->>IOS: SplitForAsu keys QUERY<br/>每批最多 queryIoNum 256
    IOS-->>TE: subBatches
    loop 每个 subBatch
        TE->>TE: AllocateRequestCid
        TE->>BM: Allocate flagBuffer<br/>Exist 16 加 (n+7)/8
        TE->>TE: BuildExistRequest keys attrs cid flagBuffer
        TE->>PM: GetPackedSize Exist
        PM-->>TE: packedSize
        TE->>BM: Allocate sendSge packedSize
        TE->>PM: PackRequest sendSge Exist req
    end
    TE->>CM: AssignSubBatchConnections<br/>SelectConnection 每个子批次
    TE->>TE: BuildSubBatchSendBuffers<br/>组装 SendIoBatch 跳过失败子批次
    TE->>TP: Send ioBatches kernelCount quietCount
    TP-->>TE: sendStatuses
    TE->>TE: 存 subBatchContexts 到 task
    TE->>TE: InitializeRemainingSubBatchCount
    TE->>TE: TryFinalizeFromSubBatches
    Note over T: ③ 未同步完成 等待 Poll

    Note over T: ④ CompletionLoop 轮询 每1ms
    T->>TE: Poll task
    loop 每个 PENDING 子批次
        TE->>TE: 读 flagBuffer 头 16B
        TE->>PM: PollResponseCid 检查 cid
        alt 无或不匹配
            Note over TE: continue 等下次轮询
        else cid 匹配
            TE->>TE: 拷贝完整 flagBuffer
            TE->>PM: UnpackResponse Exist batchNum
            PM-->>TE: KvResponse status result_buffer
            TE->>CM: ReportSuccess 或 ReportFailure
            TE->>TE: CompleteSubBatch 释放 slot --remaining
        end
    end
    TE->>TE: TryFinalizeFromSubBatches 全部完成则 COMPLETED
    Note over T: ⑤ 完成回调
    T->>TM: NotifyCompletion task
    TM->>TM: BuildResult QUERY BuildQueryResultFromEntryStatus
    TM->>T: task NotifyCompletion result 触发 onComplete
```

## 4. Load / Store 流程时序图

Load/Store 与 Query 的整体骨架一致，差异在于：

1. 入口是 `LoadAsync/StoreAsync` → `SubmitAsync(entries)`
2. 路由用 entry 的 key 做路由：`ExtractEntryKeys(entries)` → `RouteKeys`
3. Transport 侧走 **entry batch 分支**（`IsEntryBatchOp`），且 `LOAD→BATCH_LOAD`、`STORE→BATCH_STORE`
4. 子批次每批最多 `asuBatchLoadIoNum(110)` / `asuBatchStoreIoNum(110)` 个 entry
5. 需要解析每个 entry 的 MR key（`ResolveSqeMrKeys`）

```mermaid
sequenceDiagram
    autonumber
    participant User
    participant C as AsuClientImpl
    participant CTM as ClientTaskManager
    participant R as Router
    participant T as AsuTransportImpl
    participant TE as TransportTaskExecutor
    participant IOS as IoScheduler
    participant PM as ProtocolManager
    participant CM as ConnectionManager
    participant TP as TransProvider

    User->>C: LoadAsync(entries, taskId) / StoreAsync(...)
    C->>C: SubmitAsync LOAD/STORE entries
    C->>C: GetSnapshot()
    C->>CTM: Submit ClientTask LOAD/STORE entries
    C-->>User: OK taskId

    Note over C: WorkerLoop
    C->>CTM: Process task
    CTM->>CTM: BuildTransportTasks
    CTM->>R: RouteKeys ExtractEntryKeys entries
    R-->>CTM: routes asuId to indices
    Note over CTM: 映射 LOAD 到 BATCH_LOAD<br/>STORE 到 BATCH_STORE
    CTM->>T: Submit transportTask
    T->>EQ: TryPush SPSC 队列

    Note over T: WorkerLoop 消费
    T->>TE: Execute task
    TE->>TE: SubmitTaskRequests<br/>IsEntryBatchOp = true
    TE->>TE: NormalizeTransportOpType<br/>LOAD 到 BATCH_LOAD STORE 到 BATCH_STORE
    TE->>IOS: SplitForAsu entries BATCH_LOAD/BATCH_STORE<br/>每批最多 110
    IOS-->>TE: subBatches
    loop 每个 subBatch
        TE->>TE: SubmitEntrySubBatchRequest
        TE->>TE: AllocateRequestCid
        TE->>TE: ResolveSqeMrKeys entries registeredRegions<br/>查每个 entry.buffer.handle 到 tokenId
        TE->>BM: Allocate flagBuffer<br/>BatchRetrieve/Store 16 加 (n+1)/2
        TE->>TE: BuildBatchRetrieveRequest / BuildBatchStoreRequest<br/>含 buffer_addr mr_key length offset
        TE->>PM: PackRequest sendSge opcode req
    end
    TE->>CM: AssignSubBatchConnections
    TE->>TE: BuildSubBatchSendBuffers
    TE->>TP: Send ioBatches kernelCount quietCount
    TE->>TE: TryFinalizeFromSubBatches
```

> Load/Store 的响应轮询与回调路径与 Query 完全相同（见 3.1），唯一区别是 `BuildResult` 中 `task.opType != QUERY` 时 `result.queryResult.reset()`，不返回 exists 列表，而是按 entry 回填 `entryStatus`。

### 4.1 Transport 内部 Load/Store 处理时序图

聚焦 Transport 层内部：与 3.2 结构一致，差异在 entry batch 分支——多一步 `ResolveSqeMrKeys`，子批次上限 110，请求对象为 `KvBatchRetrieveRequest` / `KvBatchStoreRequest`。

```mermaid
sequenceDiagram
    autonumber
    participant T as AsuTransportImpl
    participant TM as TransportTaskManager
    participant EQ as executeQueue SPSC
    participant TE as TransportTaskExecutor
    participant IOS as IoScheduler
    participant BM as BufferManager
    participant PM as ProtocolManager
    participant CM as ConnectionManager
    participant TP as TransProvider

    Note over T: ① 入队
    T->>T: Submit 设 entryStatus deadline
    T->>TM: Submit task 分配 taskId
    T->>EQ: TryPush task

    Note over T: ② WorkerLoop 消费
    EQ-->>T: pop task
    T->>TE: Execute task
    TE->>TE: CAS PENDING 到 INFLIGHT
    TE->>TE: SubmitTaskRequests IsEntryBatchOp=true
    TE->>TE: NormalizeTransportOpType<br/>LOAD 到 BATCH_LOAD STORE 到 BATCH_STORE
    TE->>IOS: SplitForAsu entries BATCH_LOAD/BATCH_STORE<br/>每批最多 110
    IOS-->>TE: subBatches
    loop 每个 subBatch
        TE->>TE: AllocateRequestCid
        TE->>TE: ResolveSqeMrKeys entries<br/>锁 registeredRegionsMu 查 tokenId
        TE->>BM: Allocate flagBuffer<br/>BatchRetrieve/Store 16 加 (n+1)/2
        TE->>TE: BuildBatchRetrieveRequest 或 BuildBatchStoreRequest<br/>含 buffer_addr mr_key length offset
        TE->>PM: GetPackedSize opcode
        PM-->>TE: packedSize
        TE->>BM: Allocate sendSge packedSize
        TE->>PM: PackRequest sendSge opcode req
    end
    TE->>CM: AssignSubBatchConnections<br/>SelectConnection 每个子批次
    TE->>TE: BuildSubBatchSendBuffers<br/>组装 SendIoBatch 跳过失败子批次
    TE->>TP: Send ioBatches kernelCount quietCount
    TP-->>TE: sendStatuses
    TE->>TE: 存 subBatchContexts 到 task
    TE->>TE: InitializeRemainingSubBatchCount
    TE->>TE: TryFinalizeFromSubBatches
    Note over T: ③ 未同步完成 等待 Poll

    Note over T: ④ CompletionLoop 轮询 每1ms
    T->>TE: Poll task
    loop 每个 PENDING 子批次
        TE->>TE: 读 flagBuffer 头 16B
        TE->>PM: PollResponseCid 检查 cid
        alt 无或不匹配
            Note over TE: continue 等下次轮询
        else cid 匹配
            TE->>TE: 拷贝完整 flagBuffer
            TE->>PM: UnpackResponse opcode batchNum
            PM-->>TE: KvResponse status result_buffer
            TE->>CM: ReportSuccess 或 ReportFailure
            TE->>TE: CompleteSubBatch 释放 slot --remaining
        end
    end
    TE->>TE: TryFinalizeFromSubBatches 全部完成则 COMPLETED
    Note over T: ⑤ 完成回调
    T->>TM: NotifyCompletion task
    TM->>TM: BuildResult 非 QUERY 按 entry 回填 entryStatus
    TM->>T: task NotifyCompletion result 触发 onComplete
```

## 5. Transport 侧处理详解

### 5.1 双线程模型

`AsuTransportImpl` 采用生产者-消费者 + 轮询的双线程模型（见 [asu\_transport\_impl.h#L95](../trans/src/asu_transport_impl.h#L95-L98)）：

```mermaid
sequenceDiagram
    autonumber
    participant P as 生产者 Submit
    participant EQ as executeQueue SPSC
    participant WL as WorkerLoop
    participant TE as TransportTaskExecutor
    participant TM as TransportTaskManager
    participant CL as CompletionLoop
    participant CB as onComplete 回调

    P->>TM: taskManager Submit task
    P->>EQ: TryPush task
    Note over WL: WorkerLoop 线程
    EQ-->>WL: pop task
    WL->>TE: Execute task
    TE->>TE: 拆子批次 + 打包 + 下发
    TE->>TM: 存 subBatchContexts 到 task
    TE->>TE: TryFinalizeFromSubBatches
    alt 同步完成 done=true
        WL->>TM: NotifyCompletion
        TM->>CB: task NotifyCompletion result
    else 异步 done=false 等 Poll
        Note over CL: CompletionLoop 线程 每1ms遍历
        CL->>TM: 遍历所有 INFLIGHT task
        CL->>TE: Poll task
        TE->>TE: 轮询 CQE + 解包 + 完成
        alt 全部子批次完成
            CL->>TM: NotifyCompletion
            TM->>CB: task NotifyCompletion result
        end
    end
```

- **WorkerLoop**（[asu\_transport\_impl.cpp#L435](../trans/src/asu_transport_impl.cpp#L435-L441)）：从 SPSC 环形队列消费任务，调用 `Execute`。若 `Execute` 返回 true（例如全部子批次在 build 阶段就失败、无需下发），立即 `NotifyCompletion`。
- **CompletionLoop**（[asu\_transport\_impl.cpp#L443](../trans/src/asu_transport_impl.cpp#L443-L451)）：每 1ms 遍历 `taskManager_` 中所有任务，对 INFLIGHT 任务调用 `Poll` 轮询 flag buffer 中的 CQE，完成后 `NotifyCompletion`。

### 5.2 Execute 执行管线（重点）

`TransportTaskExecutor::Execute`（[transport\_task\_executor.cpp#L172](../trans/src/transport_task_executor.cpp#L172-L221)）是 Transport 侧的核心，分四个阶段：

```mermaid
sequenceDiagram
    autonumber
    participant WL as WorkerLoop
    participant TE as TransportTaskExecutor
    participant IOS as IoScheduler
    participant CM as ConnectionManager
    participant TP as TransProvider

    WL->>TE: Execute task
    TE->>TE: CAS PENDING 到 INFLIGHT

    Note over TE: 阶段1 SubmitTaskRequests
    TE->>IOS: SplitForAsu 拆子批次
    IOS-->>TE: subBatches
    TE->>TE: 打包 SQE 分配 cid 与 buffer

    Note over TE: 阶段2 AssignSubBatchConnections
    TE->>CM: SelectConnection 每个子批次
    CM-->>TE: ConnectionChannel

    Note over TE: 阶段3 BuildSubBatchSendBuffers
    TE->>TE: 组装 SendIoBatch 跳过失败子批次

    Note over TE: 阶段4 SendSubBatchBuffers
    TE->>TP: Send ioBatches 批量下发
    TP-->>TE: sendStatuses

    TE->>TE: 存 subBatchContexts 到 task
    TE->>TE: InitializeRemainingSubBatchCount
    TE->>TE: TryFinalizeFromSubBatches
    alt done=true
        TE-->>WL: true 立即 NotifyCompletion
    else done=false
        TE-->>WL: false 等 CompletionLoop 轮询
    end
```

#### 阶段 1：SubmitTaskRequests（[asu\_submit\_flow.cpp#L52](../trans/src/asu_submit_flow.cpp#L52-L112)）

按操作类型分三支：

- **EntryBatch 分支**（LOAD/STORE/BATCH\_LOAD/BATCH\_STORE）：`NormalizeTransportOpType` → `ioScheduler_.SplitForAsu(entries, opType)` → 对每个子批次 `SubmitEntrySubBatchRequest`
- **KeyBatch 分支**（QUERY/DELETE）：`ioScheduler_.SplitForAsu(keys, opType)` → `SubmitKeySubBatchRequest`
- **KeepAlive 分支**：`SubmitKeepAliveRequest`

子批次大小（[io\_scheduler.cpp#L80](../trans/src/io_scheduler.cpp#L80-L90)）：

| opType       | 子批次上限                      |
| ------------ | -------------------------- |
| BATCH\_LOAD  | `asuBatchLoadIoNum` (110)  |
| BATCH\_STORE | `asuBatchStoreIoNum` (110) |
| DELETE       | `asuDeleteIoNum` (254)     |
| QUERY        | `asuQueryIoNum` (256)      |

#### 子批次请求构建（[sqe\_request.cpp](../trans/src/sqe_request.cpp)）

`SubmitKeySubBatchRequest`（Query）/`SubmitEntrySubBatchRequest`（Load/Store）的共同步骤：

```mermaid
sequenceDiagram
    autonumber
    participant TE as TransportTaskExecutor
    participant BM as BufferManager
    participant PM as ProtocolManager

    TE->>TE: entryStatus.assign size OK
    TE->>TE: AllocateRequestCid 16bit cid

    alt Load/Store entry 分支
        TE->>TE: ResolveSqeMrKeys entries<br/>锁 registeredRegionsMu 查 tokenId
    else Query/Delete key 分支
        TE->>TE: 无需解析 MR key
    end

    TE->>BM: PrepareSubBatchRequest<br/>AllocateSubBatchFlagBuffer
    Note over BM: flag 大小 = GetFlagBufferSize<br/>Store/Retrieve: 16+(n+1)/2<br/>Delete/Exist: 16+(n+7)/8
    BM-->>TE: flagBuffer SGE

    TE->>TE: BuildSqeRequest 按 opcode 构造请求对象<br/>Exist / Delete / BatchRetrieve / BatchStore

    TE->>PM: PackSubBatchRequest
    TE->>PM: GetPackedSize opcode request
    PM-->>TE: packedSize
    TE->>BM: Allocate packedSize sendSge
    BM-->>TE: sendSge SGE

    alt 内存类型 = ASCEND_DEVICE
        TE->>PM: PackRequest staging buffer opcode request
        TE->>TE: aclrtMemcpy host 到 device<br/>packed SQE 拷到 sendSge.device_addr
    else 内存类型 = HOST_PINNED
        TE->>PM: PackRequest sendSge.local_addr opcode request
    end

    TE->>TE: subBatchContext.status = OK
```

flag buffer 大小由 `GetFlagBufferSize`（[sqe\_request.cpp#L87](../trans/src/sqe_request.cpp#L87-L96)）决定：

- BatchStore/BatchRetrieve：`16 + (n+1)/2`（每 entry 半字节状态位）
- Delete/Exist：`16 + (n+7)/8`（每 entry 1 bit）
- 其他：`16 + 1`

SQE 请求对象对应关系：

| TransportOpType | KvOpcode            | 请求类                      | flag buffer 算法 |
| --------------- | ------------------- | ------------------------ | -------------- |
| QUERY           | Exist(0xC)          | `KvExistRequest`         | `(n+7)/8`      |
| BATCH\_LOAD     | BatchRetrieve(0x46) | `KvBatchRetrieveRequest` | `(n+1)/2`      |
| BATCH\_STORE    | BatchStore(0x45)    | `KvBatchStoreRequest`    | `(n+1)/2`      |
| DELETE          | Delete(0x8)         | `KvDeleteRequest`        | `(n+7)/8`      |

#### 阶段 2-3：连接分配与 buffer 组装

`AssignSubBatchConnections`（[transport\_task\_executor.cpp#L148](../trans/src/transport_task_executor.cpp#L148-L170)）：对每个未失败的子批次，`connManager_->SelectConnection()` 选一条 channel；选不到则该子批次直接 COMPLETED + 失败。

连接组、channel cache、路由选择、错误计数和后台恢复的内部设计参见 [`connection_manager_design.md`](connection_manager_design.md)。本节只描述它在端到端 IO 流程中的调用位置。

`BuildSubBatchSendBuffers`（[asu\_submit\_flow.cpp#L114](../trans/src/asu_submit_flow.cpp#L114-L165)）：遍历子批次，跳过已失败或 buffer 未就绪的，把就绪的组装成 `TransProvider::SendIoBatch{connectionHandle, sendBuffer, flagBuffer, len}`。

#### 阶段 4：Send（[asu\_submit\_flow.cpp#L167](../trans/src/asu_submit_flow.cpp#L167-L204)）

调用 `transProvider_->Send(ioBatches, kernelCount, quietCount)` 一次性批量下发所有子批次。底层 provider（AICPU/AIV/Fake）负责实际硬件下发，返回逐个 `Status`。失败的子批次置 COMPLETED 并 `ReportFailure(channel)`。

### 5.3 Poll 轮询与 CQE 解包（[transport\_task\_executor.cpp#L223](../trans/src/transport_task_executor.cpp#L223-L334)）

```mermaid
sequenceDiagram
    autonumber
    participant CL as CompletionLoop
    participant TE as TransportTaskExecutor
    participant PM as ProtocolManager
    participant CM as ConnectionManager

    CL->>TE: Poll task

    alt deadline 超时
        TE->>TE: 全部子批次置 TIMEOUT
        TE->>CM: ReportFailure channel
        TE->>TE: state = COMPLETED
    else 未超时
        loop 每个 PENDING 子批次
            TE->>TE: 读 flag buffer 头 16B<br/>device 到 host 或直接 host
            TE->>PM: PollResponseCid 检查 CQE 的 cid
            alt 无或不匹配
                Note over TE: continue 等下次轮询
            else cid 匹配
                TE->>TE: 拷贝完整 flagBuffer device 到 host
                TE->>PM: UnpackResponse 按 opcode 解 CQE
                PM-->>TE: KvResponse status result_buffer
                TE->>TE: 映射状态 FillEntryStatusFromCqeResult
                alt 内部错误或 IO_TIMEOUT
                    TE->>CM: ReportFailure
                else 成功或其他
                    TE->>CM: ReportSuccess
                end
                TE->>TE: CompleteSubBatch 释放资源 --remaining
            end
        end
        TE->>TE: TryFinalizeFromSubBatches 全部完成则 COMPLETED
    end
```

关键点：

- **cid 匹配机制**：每个子批次有唯一 16bit `cid`，下发后写进 SQE；ASU 硬件完成时把同一 `cid` 写进 flag buffer 头部 CQE。Poll 通过 `PollResponseCid` 比对 `completedCid == subBatchContext.cid` 判断该子批次是否完成。
- **设备内存处理**：flag buffer 若为 `ASCEND_DEVICE`，需先 `aclrtMemcpy` 拷 16B 头判断 cid，匹配后再拷完整 buffer 解包（避免每次全量拷贝）。
- **超时**：`task->deadline` 由 `Submit` 时按 `config_.timeoutMs` 计算，Poll 中检测到超时则把所有 PENDING 子批次置 `TIMEOUT`。
- **查询结果特殊处理**：`ASU_CQE_CHECK_RESULT_BUFFER` 对 Query 是"需检查 result buffer"的正常语义，会被转成 `Status::OK()` 而非错误（[transport\_task\_executor.cpp#L315](../trans/src/transport_task_executor.cpp#L315-L320)）。

### 5.4 完成与回调链

```mermaid
sequenceDiagram
    autonumber
    participant Src as Poll 或 Execute 返回 done
    participant TTM as TransportTaskManager
    participant Task as TransportTask
    participant CB as onComplete 回调
    participant CTM as ClientTaskManager

    Src->>TTM: NotifyCompletion task
    TTM->>TTM: BuildResult 汇总 finalStatus entryStatus<br/>QUERY BuildQueryResultFromEntryStatus
    TTM->>Task: NotifyCompletion result
    Task->>CB: 调用 onComplete 回调
    CB->>CTM: CompleteTransportTask clientTask idx result
    CTM->>CTM: QUERY 散列 exists 到原 index<br/>回填 entryStatus
    CTM->>CTM: --remainingTransportTasks
    alt 归零
        CTM->>CTM: Finalize<br/>anyFailed 到 PARTIAL_FAILED<br/>state = COMPLETED cv.notify
    else 未归零
        Note over CTM: 继续等待其他 ASU
    end
    TTM->>TTM: Remove taskId 从 transport taskManager 移除
```

### 5.5 资源回收

- `ReleaseSubBatchContext`（[transport\_task\_executor.cpp#L78](../trans/src/transport_task_executor.cpp#L78-L104)）：归还 send buffer slot、flag buffer slot、`channel->ReleaseInflight()`。
- 在 `Execute`/`Poll`/`Cancel` 各路径都会对已完成或失败的子批次调用 `ReleaseSubBatchResources`，避免 slot 泄漏。
- `Cancel`（[transport\_task\_executor.cpp#L124](../trans/src/transport_task_executor.cpp#L124-L139)）：把所有 entry 置失败状态，释放全部子批次资源，置 COMPLETED。注意"Best-effort cancellation, does not interrupt underlying UB/RoCE IO"（[asu\_transport.h#L114](../trans/include/asu_transport/asu_transport.h#L114)）——只是放弃等待，不中断已下发的硬件 IO。

## 6. 两条流水线对比汇总

| 维度              | Query                       | Load / Store                           |
| --------------- | --------------------------- | -------------------------------------- |
| Client 入口       | `QueryAsync(keys, options)` | `LoadAsync/StoreAsync(entries)`        |
| ClientOpType    | QUERY                       | LOAD / STORE                           |
| 路由输入            | keys                        | `ExtractEntryKeys(entries)`            |
| TransportOpType | QUERY                       | BATCH\_LOAD / BATCH\_STORE             |
| KvOpcode        | Exist(0xC)                  | BatchRetrieve(0x46) / BatchStore(0x45) |
| 子批次上限           | 256 (queryIoNum)            | 110 (asuBatchLoad/StoreIoNum)          |
| flag buffer 算法  | `16+(n+7)/8`                | `16+(n+1)/2`                           |
| MR key 解析       | 不需要（无 entry buffer）         | `ResolveSqeMrKeys` 查 tokenId           |
| 结果              | `queryResult.exists`（0/1）   | `entryStatus`（按 entry）                 |
| 完成回填            | 散列 exists 到 originalIndices | 散列 entryStatus 到 originalIndices       |

## 7. 端到端数据流总览

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户
    participant CLI as AsuClientImpl
    participant CTM as ClientTaskManager
    participant R as Router
    participant TR as AsuTransportImpl
    participant TE as TransportTaskExecutor
    participant TP as TransProvider
    participant HW as ASU 硬件
    participant CL as CompletionLoop

    U->>CLI: QueryAsync / LoadAsync / StoreAsync
    CLI->>CTM: SubmitAsync 建 ClientTask
    CLI->>CLI: WorkerLoop 取出 task
    CLI->>CTM: Process task
    CTM->>R: RouteKeys 拆分
    R-->>CTM: TransportTask × N 个 ASU
    CTM->>TR: Submit 设 onComplete
    TR->>TR: SubmitTask 入队 executeQueue
    TR->>TE: WorkerLoop 消费 Execute
    TE->>TE: SplitForAsu 拆子批次
    TE->>TE: Build + Pack SQE 分配 buffer
    TE->>TP: AssignConnections + Send 批量下发
    TP->>HW: 硬件下发
    HW-->>HW: CQE 写入 flagBuffer
    CL->>TE: 每 1ms Poll
    TE->>TE: PollResponseCid 匹配 cid
    TE->>TE: UnpackResponse + CompleteSubBatch
    TE->>CTM: NotifyCompletion onComplete
    CTM->>CTM: 散列回填 + 聚合
    CTM->>CTM: Finalize COMPLETED
    CTM-->>U: Wait 返回 TaskResult
```
