# SGLang UCM Linker 的设备池分组与 ASU 布局

以 GLM-5 DSA 为例，SGLang 提供 KV 和 Indexer 两个设备池条目。UCM 把它们合成一个 Store group，每个 worker 为这个 group 创建一个 Store 实例。ASU 对同一页使用一个远端 key，先放所有层的 KV，再放所有层的 Indexer。

## SGLang 提供页索引和设备池

SGLang 的 `_build_dsa_device_pool_group()` 给两个 `DevicePoolEntry` 分别设置 `name=KV` 和 `name=INDEXER`，两者的 `indices_from_pool` 都是 `KV`，并共用同一份 `layer_mapping`。`DevicePoolGroup.resolve_transfers()` 用 `indices_from_pool` 找到逻辑 transfer，再用各自的 `name` 生成物理 transfer，复制源页的 `keys`，按对应池的行规则转换设备索引。

因此同一页在两条 transfer 中有同一个 `page_key`，实际地址分别由 KV 的 `kv_buffer` 和 Indexer 的 `index_k_with_scale_buffer` 计算。KV 池按 token 行寻址，Indexer 池按 page 行寻址。 [SGLang 设备池组装和 transfer 展开](</D:/a_storage/1_sglang/sglang_zy/sglang/python/sglang/srt/mem_cache/hybrid_cache/linker_pool_assembler.py>)

## GLM-5 DSA 的两个设备池如何创建一个 Store

下图以一个 SGLang worker 为例。该 worker 有 78 层，页大小为 64 token。

```{mermaid}
flowchart TD
    KV["SGLang KV 设备池<br/>页索引来源 KV · 每页每层 73,728 B"]
    IDX["SGLang Indexer 设备池<br/>页索引来源 KV · 每页每层 8,448 B"]
    LINKER["UcmDirectLinker<br/>索引来源和层映射相同<br/>78 层共用长度表 [8448, 73728]"]
    GROUP["1 个 Store group<br/>kv_82176x78<br/>地址列 [Indexer, KV]"]
    ADAPTER["UcmDirectStoreAdapter<br/>按 store_group 汇总 2 条池注册记录"]
    PIPELINE["创建 1 个 UcmPipelineStore<br/>内部装 1 个 ASU Store"]
    PAGE["同一页对应 1 个 ASU key<br/>先存所有层的 KV，再存所有层的 Indexer"]
    KV --> LINKER
    IDX --> LINKER
    LINKER --> GROUP --> ADAPTER --> PIPELINE --> PAGE
```

1. **先合设备池。** `_build_pool_bundles()` 用 `(indices_from_pool, 完整 layer_mapping)` 作为第一轮分组键。这个例子的 KV 和 Indexer 都从 `KV` 取索引，层映射都是 `0 → 0, …, 77 → 77`，因此落到同一个分组。
2. **再按层的长度表拆分。** 组内设备池按 `name` 排成 `[indexer, kv]`。第 0 层的长度表是 `[8448, 73728]`，第 1 层到第 77 层也是这一组值。`by_schema` 因此只有一个键，对应 78 层，最终只产出一个 bundle。每层载荷为 `8,448 + 73,728 = 82,176` 字节，所以 `store_group` 叫 `kv_82176x78`。若换模型后某些层的长度表不同，这一步会把这些层拆到各自的 bundle。 [Linker 分组](../../../ucm/integration/sglang/ucm_direct_linker.py#L331)
3. **按 group 创建 Store。** `_pool_metadata()` 仍生成两条记录，`pool_name` 分别是 `kv` 和 `indexer`，两条的 `store_group` 都是 `kv_82176x78`。Adapter 把它们放进同一个 `by_group` 桶，只对这个桶调用一次 `_ensure_store_schema()`。它首次调用 `create_connector()` 后将实例保存到 `_stores[store_group]`，后续直接复用。因此这个 worker 创建 **1 个 `UcmPipelineStore`**，配置 `store_pipeline: ASU` 时里面装 **1 个 ASU Store**。 [池元数据](../../../ucm/integration/sglang/ucm_direct_linker.py#L286) · [Adapter 注册](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L251) · [Store 创建与复用](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L409) · [ASU pipeline](../../../ucm/store/pipeline/connector.py#L296)

两条池记录仍各自提供显存地址。`_page_transfers()` 检查组内页 key 和命中策略一致，然后按 `[Indexer, KV]` 填地址列。Adapter 给 Store 传入 `tensor_layout="sglang_l1"`、`tensor_size_list=[8448, 73728]`、`shard_size=82176` 和 `block_size=6409728`。 [地址列构建](../../../ucm/integration/sglang/ucm_direct_linker.py#L506) · [Store 配置](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L469)

`page_key` 标识逻辑页。Adapter 将缓存身份、`store_group` 和 `page_key` 一起哈希成 `block_id`，ASU 再用 `sglang_l1` 布局版本和 `block_id` 生成远端 key。同一个页如果属于多个 Store group，就会生成各组自己的 block ID 和远端 key。ASU 的 namespace 从 `kv_ns_ids` 配置取得，group 数量和 GPU 池数量各自独立于 namespace 配置。 [Block ID 生成](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L540) · [ASU key](../../../ucm/store/asu/cc/asu_store.cc#L99) · [Namespace 配置](../../../ucm/store/asu/cc/asu_store.cc#L211)

## ASU 把地址列映射到远端 offset

一条 Store shard 记录对应某页在当前 group 的某个层位置，包含 `owner`（页的 block ID）、`index`（组内层下标）和 `addrs`（显存地址数组）。例如 78 层 DSA 的 `tensor_size_list` 是 `[8448, 73728]` 字节，每个 shard 都用同一张长度表。组包含部分模型层时，Adapter 负责把逻辑层号映射成组内 shard 下标。 [逐层地址与 shard 选择](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L834)

ASU 的 `BuildKvBuffers()` 按 `tensorIndex` 同时取 `shard.addrs`、`tensorSizes` 和 `tensorOffsets`，每个地址片段生成一条 I/O entry，并按地址范围找到已注册的显存句柄。`BuildSglangL1TensorOffsets()` 倒序访问地址列，为每一列预留覆盖全部 shard 的连续范围，片段步长按 512 字节向上对齐。当前 DSA 的输入列为 `[Indexer, KV]`，因此远端先放 KV。 [ASU offset](../../../ucm/store/asu/cc/asu_store.cc#L856) · [I/O entry 构建](../../../ucm/store/asu/cc/asu_store.cc#L974)

| `tensorIndex` | 显存地址列 | 实际 I/O 长度 | 该页在组内第 `s` 层的远端 offset |
| --- | --- | ---: | --- |
| `0` | Indexer | 8,448 B | `78 × 73,728 + s × 8,704` |
| `1` | KV | 73,728 B | `s × 73,728` |

```text
同一个 ASU key
[KV L0][KV L1] … [KV L77][Indexer L0][Indexer L1] … [Indexer L77]
                         ↑ 5,750,784 B
```

Adapter 日志里的 `page_block_bytes=6409728` 是整页实际载荷。ASU 计算远端偏移时把 Indexer 的步长对齐到 8,704 字节，fake provider 每次仍按实际片段长度传输。其他模型沿用按元数据和列位置计算的规则。远端列顺序由输入列的逆序决定，具体长度和支持的对齐条件取决于该模型的设备池和 provider。 [ASU 对齐和长度校验](../../../ucm/store/asu/cc/asu_store.cc#L657)

## PUT、逐层 GET 和 entry 数量

- **普通 PUT** 将一个 group 内的地址按「shard × 页」展开，整个 group 一次提交 `Dump`，ASU 再为每行的每个地址列生成 I/O entry。若同一次提交包含 93 页、78 层，每层有 KV 和 Indexer 两个片段，就有 `93 × 78 × 2 = 14,508` 条 entry。在单 ASU、每批最多 110 条的条件下，KvClient 会分成 132 个子批次。 [Adapter 整组 PUT](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L774) · [ASU 提交](../../../ucm/store/asu/cc/asu_store.cc#L546) · [KvClient 拆批](../../../kv_semantics/src/trans/task/io_scheduler.cc#L37) · [默认每批上限](../../../kv_semantics/include/kv_types.h#L224)
- **逐层 GET** 对所需的逻辑层取各页对应 shard 的地址。20 页的一层会生成 `20 × 2 = 40` 条 entry，连续加载 30 层总共生成 1,200 条。GET 和 PUT 用同一套 offset 计算。KV 在前和按层穿插只改变远端 offset，当前 ASU 路径的 entry 数由提交的页、shard 和地址片段数决定。 [Adapter 逐层 GET](../../../ucm/integration/sglang/ucm_direct_store_adapter.py#L826) · [ASU I/O entry 构建](../../../ucm/store/asu/cc/asu_store.cc#L974)
- **`Cache|Posix`** 复用相同的 group 和地址列。Cache 按片段长度处理设备侧地址，Posix 收到的 `tensor_size` 设为整条 shard 的大小，按连续 shard 处理缓存文件。两种 pipeline 共享 Linker 的提交格式，各自决定后端 I/O 的粒度。 [Cache 与 Posix 的配置](../../../ucm/store/pipeline/connector.py#L232)
