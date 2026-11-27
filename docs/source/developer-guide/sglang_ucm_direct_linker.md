# SGLang UcmDirectLinker 当前实现

`UcmDirectLinker` 是 SGLang Unified Radix Cache 到 UCM Store 的直连实现。**每个 Linker 实例有一条加载队列、一条卸载队列，以及各自的 Python worker。** 查询在调用侧同步执行。加载按层把缓存恢复到设备，普通卸载按缓存树节点提交该节点涉及的所有层。本文对应 UCM `b0012d38` 的现有代码，与 [逐层 PUT 设计稿](sglang_ucm_layerwise_put_design.md) 配套阅读。

## 从 SGLang 接到 UCM

SGLang 的动态 Linker 配置指定模块 `ucm.integration.sglang.ucm_direct_linker`、类 `UcmDirectLinker` 和 `linker_extra_config.UCM_CONFIG_FILE`。在 v0.5.20 兼容路径上，UCM 的 registry patch 创建该 Linker，交给 `cache.init_cache_linker()`，并把它的 `LayerWiseLoadCounter` 注册给设备 KV pool 和 TP worker。SGLang 侧缓存树的 `UnifiedCacheLinkerWrapper` 负责请求匹配、源或目标设备页归属、节点锁和 TP 结果汇总。 [动态入口示例](../../../examples/sglang_asu_mix.sh#L33) · [v0.5.20 registry patch](../../../ucm/integration/sglang/patch/v0520/registry_patch.py#L65)

```{mermaid}
flowchart LR
    W["SGLang 缓存树与 Wrapper"]
    L["UcmDirectLinker"]
    LQ["_load_queue"]
    OQ["_offload_queue"]
    LW["load worker"]
    OW["offload worker"]
    A["UcmDirectStoreAdapter"]
    S["每个 Store group 一个 Store 实例"]
    W --> L
    L -->|"同步 lookup"| A
    L -->|"启动加载批次"| LQ --> LW --> A
    L -->|"接纳节点卸载"| OQ --> OW --> A
    A --> S
    LW -.->|"逐层就绪与加载收尾"| W
    OW -.->|"按节点接收顺序发布结果"| W
```

| Linker 状态 | 谁写入 | 内容与用途 |
| --- | --- | --- |
| `_pending_loads` | `load(rid, transfers)` | Scheduler 侧暂存表，按 `rid` 保存待启动的加载 |
| `_load_queue` | `start_layer_wise_loading()` 或 `prefetch()` | 普通加载批次含 counter index、请求映射和 GPU 事件。主动预取含 transfers、事件和 Future |
| `_completed_loads` / `_load_results` | load worker | 旧接口通过结果队列交还一批 `rid`，支持显式 finish 的接口以 counter index 对应的 Future 返回批次结果 |
| `_offload_queue` | `offload(transfers)` | 一次已接纳的树节点卸载，保存本 rank 的 transfers 和 GPU 事件 |
| `_offload_results` | offload worker | 每次已接纳的卸载对应一个布尔结果，顺序与接纳顺序一致 |

上述两条工作队列由构造函数创建，线程名分别是 `ucm-load-tp{rank}` 和 `ucm-offload-tp{rank}`。两条路径共用 Adapter。当前普通 PUT 的提交、轮询和句柄回收都在同一个 offload worker 中执行。 [队列与线程初始化](../../../ucm/integration/sglang/ucm_direct_linker.py#L138) · [线程启动](../../../ucm/integration/sglang/ucm_direct_linker.py#L198)

## 初始化与数据描述符

Linker 从 SGLang allocator 取得设备 KV cache，调用 `resolve_hybrid_device_pool_group()` 得到设备池条目。每个条目包含索引来源、逻辑层映射和设备 buffer 元数据。Linker 先按相同的索引来源和层映射归组，再按每层的片段长度表拆分，最后得到 `_PoolBundle`。同一 bundle 的每个 shard 使用相同的片段长度和顺序，片段按 pool 名再按条目内部顺序排列。 [设备池与 bundle](../../../ucm/integration/sglang/ucm_direct_linker.py#L124) · [分组规则](../../../ucm/integration/sglang/ucm_direct_linker.py#L331)

| 对象 | 负责表示的内容 |
| --- | --- |
| `PoolTransfer` | SGLang 的逻辑 pool、按页排列的 key、设备 slots 和命中策略 |
| `_PoolBundle` | 可以在同一 Store group 中存放的一组 pool 与层 |
| `UcmLayerTransfer` | 一个 bundle 的一次页范围操作，包含 `layers`、`page_keys`、片段大小和地址描述符 |
| `block_id` | 某 Store group 中的一页，包含该组负责的全部层 |
| Store shard | 该 block 中的一层，编号等于逻辑层在 `transfer.layers` 中的位置 |

`_expand()` 让 SGLang pool group 根据索引来源展开逻辑 transfer。随后 `_page_transfers()` 按 bundle 组装同一页的各池片段，并检查组内的页 key 和命中策略一致。普通 PUT 构造形状为 `[layer, page, fragment]` 的设备地址数组。逐层 GET 保存按层生成地址的函数。查询只构造页 key 和布局描述。例如 DSA 的 KV 和 Indexer 在满足相同层映射条件时共用一个 bundle，某页某层的 shard 包含该层的两池数据。 [页描述符构造](../../../ucm/integration/sglang/ucm_direct_linker.py#L506) · [设备池与 ASU 布局详解](sglang_ucm_pool_and_asu.md)

初始化时 Linker 将 bundle 对应的 `UcmPoolMetadata` 交给 Adapter，包含页大小、层 schema、容量估算和设备 buffer 范围。Adapter 从 YAML 取一个 `ucm_connectors` 条目，按 group 建 Store，计算 `tensor_size_list`、`shard_size` 和 `block_size`，配置后端目录、显存注册范围及所属 Cache 域。block ID 是由模型和 rank 身份、group 名及页 key 生成的 16 字节 MD5。TP 切分时 block 身份包含 TP rank，TP 复制时各 rank 共享 block 身份。Store 的 `unique_id` 用于区分实际布局和共享域。 [读取配置](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L161) · [注册设备池](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L243) · [Store 配置与 block ID](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L409)

## 查询与可恢复页边界

SGLang 先匹配设备树。对于剩余的完整页，Wrapper 计算链式页 hash，构造待查的 `PoolTransfer`，调用 `lookup(rid, transfers)`。返回值是**本 rank 可恢复的页边界列表**。Wrapper 汇总各 rank 的边界并选共同边界，再保存 hit marker 给后续加载使用。共享 Cache 域识别成功时，域内查询 owner 查 Store，其他 rank 返回全部候选边界作为交集的中性结果。 [Linker 查询入口](../../../ucm/integration/sglang/ucm_direct_linker.py#L613) · [共享域识别](../../../ucm/integration/sglang/ucm_direct_linker.py#L209)

Adapter 把候选页转换成每个 group 的 block ID，逐组收窄候选边界：

- `ALL_PAGES` 用 `store.lookup_on_prefix()` 计算从起点开始的连续命中范围。Store 返回最后命中的下标，Adapter 加一得到页数。同一 `rid` 的页序列和布局可续查时，会重查上次最后一个命中页并继续扫描后缀。该边界消失时从头重查。更早的页如果被单独淘汰，续查有可能保留过长的前缀。
- `TRAILING_PAGES` 用 `store.lookup()` 取得候选范围内逐页存在性，按该 pool 要求的尾部窗口过滤每一个边界。结果可以是稀疏的边界列表。 [Adapter 命中策略](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L566)

查询接口只传 block ID。实际命中由 Store 决定。`Cache|Posix` 的 Cache 层当前用 `Exist(block, 0)` 查 shard 0 的槽位，Cache miss 再查 Posix。Posix 查看已归档文件。直连 `ASU` 调用 `KVClient::QueryAsync()`，由所使用的 provider 给出查询结果。 [Cache 查询](../../../ucm/store/cache/v2/buffer_manager.h#L48) · [Posix 查询](../../../ucm/store/posix/cc/space_manager.cc#L179) · [ASU 查询](../../../ucm/store/asu/cc/asu_store.cc#L923)

## 加载 从已命中页逐层读回设备

Wrapper 消费 hit marker，准备目标 slots，把加载的目标路径挂到缓存树并持节点锁，然后调用 `load()`。Linker 先将请求放在 `_pending_loads`，等 `start_layer_wise_loading()` 把本轮请求一起送入加载队列。此时分配 counter index，前向通过 counter 等自己要读的层。无待加载请求时返回 `-1`。 `cancel_queued_load(rid)` 可以撤销仍在暂存表中的请求。 [加载接纳](../../../ucm/integration/sglang/ucm_direct_linker.py#L681) · [启动加载批次](../../../ucm/integration/sglang/ucm_direct_linker.py#L768)

```{mermaid}
sequenceDiagram
    participant W as SGLang Wrapper
    participant L as UcmDirectLinker
    participant Q as load queue
    participant R as load worker
    participant S as Adapter 与 Store
    participant C as 层 counter
    W->>L: load(rid, transfers)
    L->>L: 按 rid 暂存在 pending loads
    W->>L: start_layer_wise_loading()
    L->>Q: 入队批次、counter index 和事件
    L-->>W: 返回 counter index
    R->>Q: 取加载批次
    R->>S: 准备 block IDs 与层映射
    loop 每个逻辑层
        R->>S: 提交该层读取
        opt 队首层的所有读取均已完成
            R->>S: wait 消费句柄
            R->>C: 该层完成，前向可以使用
        end
    end
    R->>S: 按层等待剩余任务
    R-->>W: 批次加载结果
```

worker 先等待索引事件，再准备加载描述符。它依次提交各逻辑层，并在两次提交间尝试回收已经就绪的最早层。某层所有 bundle 的任务都成功回收后，才把该层标为完成。余下任务在提交阶段结束后按层等待。异常时先回收仍在途的句柄，再报告 counter 失败。支持 `finish_layer_wise_loading()` 的 SGLang 版本会消费批次 Future。旧接口走 `_completed_loads` 的 `rid` 列表，供 Wrapper 收尾。 [逐层加载 worker](../../../ucm/integration/sglang/ucm_direct_linker.py#L791)

`prefetch(transfers)` 也进入 `_load_queue`。它走全层恢复并返回 Future，供调用方在批次设备写入收尾后取结果。

## 卸载 树节点触发普通 PUT

SGLang 的 write-through 路径从树节点生成 transfers。Wrapper 先锁节点，再调用 `offload()`。Linker 展开 transfers 并确定本 rank 的写入页。TP 切分时各 rank 写自己的数据。TP 复制时默认由 attention TP 的 rank 0 写。识别到多个 rank 共享同一 Cache 域时，使用 KV 索引的 pool 按整页切分到各 rank，独立索引的辅助 pool 交给按尾页 key 选出的 rank。即使本 rank 没分到页，也提交一个空项来维持所有 rank 的节点结果顺序。 [卸载分工与接纳](../../../ucm/integration/sglang/ucm_direct_linker.py#L895)

```{mermaid}
sequenceDiagram
    participant W as SGLang Wrapper
    participant L as UcmDirectLinker
    participant Q as offload queue
    participant O as offload worker
    participant A as Adapter 与 Store
    W->>W: 锁定缓存树节点
    W->>L: offload(transfers)
    L->>Q: 入队本 rank 的页和 GPU 事件
    L-->>W: 返回已接纳
    O->>Q: 取卸载请求
    O->>A: 按 group 提交所选页的所有 shard
    A-->>O: 返回 Store task 句柄
    Note over Q,O: 先前节点在途时仍可接收并提交后续节点
    O->>A: check 探测完成，再用 wait 回收
    O->>L: 按接纳顺序发布已完成的节点结果
    L-->>W: pop_completed_offload()
    W->>W: TP 汇总，按结果更新树并解锁
```

卸载 worker 为一次节点卸载的每个 bundle 提交一个批量 `Dump`。它保留 transfers、地址数组和事件，并把句柄放到本线程的 `in_flight` 中。后续节点可以先提交或先结束，`_offload_results` 只发布从队首开始连续完成的结果。提交失败时发布失败结果，轮询或等待出错时会收尾已有句柄。Wrapper 根据 TP 汇总结果处理外部副本状态并解锁。 [卸载 worker 与结果 FIFO](../../../ucm/integration/sglang/ucm_direct_linker.py#L976)

卸载完成标记沿用 Store task 的 `Wait` 语义。对于 `Cache|Posix`，Cache 完成设备侧数据搬运并提交后端写入后即可完成上层任务，Posix 写入的等待由 Cache 内部的后台线程处理。 [Cache 写入路径](../../../ucm/store/cache/v2/dump_queue.h#L166) · [下游 Wait](../../../ucm/store/cache/v2/dump_queue.h#L273)

## Adapter 如何把请求提交给 Store

| 环节 | Adapter 的工作 |
| --- | --- |
| 准备 | 校验布局和地址形状，缓存 `shards_by_layer`，将页 key 编码为 block IDs |
| 逐层 GET | 查逻辑层对应的 shard 编号，为该层生成设备目标地址，调用 `load_data()` |
| 普通 PUT | 按 shard 再按 page 展平地址，从行号推导每行的 block ID 和 shard index，调用 `dump_data()` |
| 内置 PipelineStore | 复用预编码的 ID 数组，直接调用原生 `Load` / `Dump` |
| 自定义 Store | 通过公开的 `load_data()` / `dump_data()` 接口提交 |
| 完成处理 | 用 `_StoreTask` 保存 Store、句柄和地址数组。 `check()` 探测状态，`wait()` 消费句柄。批量 wait 会尝试收尾全部句柄，再报告遇到的第一个错误 |

GPU 事件由 Linker 管理。地址准备会读取设备索引，因此先等索引就绪。现有 Adapter 仅在内置 `Cache|Posix` 路径支持透传可用的原生前向事件句柄，交给 Store 拷贝流等待。其他路径由 Linker 在提交前同步前向事件。 [事件依赖](../../../ucm/integration/sglang/ucm_direct_linker.py#L739) · [Adapter 准备与原生路径](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L646) · [GET 和 PUT 提交](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L774) · [任务回收](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L899)

## 重置与关闭

`reset()` 清理查询续查状态和未启动的加载，等待已入队的加载及卸载工作收尾，再清理完成结果并重置层 counter。`close()` 在 reset 之后向两条工作队列各放一个结束信号，join 两个 worker，最后由 Adapter 清除各 group 的运行状态并调用 Store 提供的 `close()`。 [Linker 关闭顺序](../../../ucm/integration/sglang/ucm_direct_linker.py#L1102) · [Adapter 关闭 Store](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L914)
