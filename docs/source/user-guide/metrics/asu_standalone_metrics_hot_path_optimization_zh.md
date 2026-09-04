# ASU Standalone Metrics 热路径优化

## 1. 文档目的

本文说明 ASU standalone metrics 在业务打点热路径上的四项性能优化，包括优化前后的差异、对应代码、并发正确性和生命周期约束。

这些优化的目标是降低 metrics 对 client/transport I/O 路径的固定 CPU 开销和尾延迟干扰。它们不改变指标名称、类型、单位、Histogram buckets 或 Prometheus/Grafana 查询语义。

## 2. 完整数据链路

```text
ASU 业务代码
  → Metrics::UpdateBuiltinBatch()
  → 原子读取 standalone backend
  → 获取当前业务线程的 ThreadBuffer
  → WriteGuard 进入当前写槽
  → 一次循环更新 N 个指标
  → WriteGuard 退出写槽

后台 aggregator
  → 切换每个 ThreadBuffer 的写槽
  → 等待旧写槽中已经开始的写入结束
  → 把旧写槽合并到进程级累计 snapshot
  → 清空旧写槽供下次复用

HTTP exporter
  → GET /metrics
  → 读取累计 snapshot
  → 输出 Prometheus exposition format
```

高频业务线程只写线程专属的增量 buffer。低频 aggregator 负责跨线程汇总，HTTP 线程只读取累计 snapshot，不会在 scrape 时清空业务数据。

## 3. 优化一：去掉每次打点的 `shared_ptr` 原子引用计数

### 3.1 优化前

facade 使用全局 `shared_ptr<MetricsBackend>` 保存 backend。每次打点通过 `atomic_load(shared_ptr)` 获得一份临时所有权：

```cpp
if (!IsEnabled()) { return; }
auto backend = LoadBackend();
if (backend) { backend->UpdateBuiltinBatch(updates, count); }
```

这会在每次打点时执行：

1. 检查 enabled 状态；
2. 原子增加 `shared_ptr` control block 的 strong reference；
3. 调用 backend；
4. 临时 `shared_ptr` 析构时原子减少 strong reference。

多个业务线程更新同一个引用计数 cache line，会产生额外的跨核一致性流量。

### 3.2 当前实现

facade 分开保存所有权和热路径访问指针：

```cpp
std::shared_ptr<MetricsBackend> gBackend;
std::atomic<MetricsBackend*> gBackendFast{nullptr};
```

- `gBackend` 持有 backend，负责对象生命周期；
- `gBackendFast` 不持有对象，只供业务线程快速访问。

打点入口只执行一次 atomic raw-pointer load：

```cpp
void UpdateBuiltinBatch(const BuiltinMetricUpdate* updates, std::size_t count) noexcept
{
    if (updates == nullptr || count == 0) { return; }
    auto* backend = LoadBackendFast();
    if (backend) { backend->UpdateBuiltinBatch(updates, count); }
}
```

`Initialize()` 先让 `gBackend` 接管所有权，再发布 `gBackendFast`。`Shutdown()` 先把 `gBackendFast` 置空，阻止新的打点进入，再 flush、stop 并释放 owner。

### 3.3 收益与约束

收益：

- 每次打点不再增加和减少 backend 的引用计数；
- metrics 未启用时只需一次指针读取和空指针判断；
- 降低多核业务线程对同一个 control block cache line 的争用。

生命周期约束：所有可能打点的业务线程必须先停止，再调用 `Metrics::Shutdown()`。raw pointer 不提供与任意并发 Shutdown 的对象保活能力。

对应代码：

- `ucm/transport/kv/asu/metrics/src/metrics.cpp`：`gBackend`、`gBackendFast`、`LoadBackendFast()`、`Initialize()`、`Shutdown()` 和 facade 打点入口。

## 4. 优化二：去掉每次打点的 slot mutex

### 4.1 为什么可以去锁

standalone collector 为每个首次打点的业务线程创建独立 `ThreadBuffer`。正常情况下：

- 一个 `ThreadBuffer` 只有所属业务线程写；
- aggregator 是唯一读取者；
- 不同业务线程不会并发修改同一个 `ThreadBuffer`。

因此不需要用 mutex 处理多个业务 writer 之间的竞争，只需要协调一个 writer 和 aggregator。

### 4.2 双槽结构

每个线程的 buffer 包含两个增量槽：

```cpp
std::atomic<int> writeIndex{0};
std::atomic<int> activeWriteIndex{kNoActiveWriter};
DeltaSlot slots[2];
```

- `writeIndex`：新打点应该写入哪个槽；
- `activeWriteIndex`：writer 当前是否正在写，以及正在写哪个槽；
- `slots[2]`：业务线程和 aggregator 交替使用的两个增量区。

### 4.3 `WriteGuard` 进入写槽

业务线程创建 `WriteGuard` 时调用 `BeginWrite()`：

```cpp
while (true) {
    const int index = writeIndex.load(std::memory_order_acquire);
    activeWriteIndex.store(index, std::memory_order_release);
    if (writeIndex.load(std::memory_order_acquire) == index) {
        return index;
    }
    activeWriteIndex.store(kNoActiveWriter, std::memory_order_release);
}
```

两次读取 `writeIndex` 用来关闭下面的竞争窗口：

```text
writer 第一次读到 slot 0
aggregator 把 writeIndex 从 0 切到 1
writer 宣布准备写 slot 0
```

writer 宣布活动槽后会再次检查 `writeIndex`。如果发现 aggregator 已经切槽，就撤销本次进入并重试；只有两次读取一致时才真正修改槽内数据。

写入完成后，`WriteGuard` 析构并调用：

```cpp
activeWriteIndex.store(kNoActiveWriter, std::memory_order_release);
```

RAII 保证函数从正常路径返回时都会退出写状态。

### 4.4 aggregator 切槽

aggregator 汇总一个 `ThreadBuffer` 时执行：

```cpp
const int oldSlot = buffer->SwitchWriteSlot();
buffer->WaitUntilInactive(oldSlot);
MergeAndReset(buffer->slots[oldSlot]);
```

时序如下：

```text
初始：writer 写 slot 0
  1. aggregator 把 writeIndex 切换到 slot 1
  2. 新打点开始写 slot 1
  3. aggregator 等待已经进入 slot 0 的 writer 退出
  4. aggregator 合并并清空 slot 0
```

等待发生在低频 aggregator 线程。业务 writer 不需要等待旧槽的合并过程，正常热路径只有少量 atomic load/store，没有 mutex lock/unlock。

对应代码：

- `ucm/transport/kv/asu/metrics/src/standalone_metrics_backend.cpp`：`DeltaSlot`、`ThreadBuffer`、`WriteGuard`、`BeginWrite()`、`EndWrite()`、`SwitchWriteSlot()`、`WaitUntilInactive()` 和 `Aggregate()`。

## 5. 优化三：去掉每次 `weak_ptr::lock()`

### 5.1 优化前

TLS cache 原来保存 `weak_ptr<ThreadBuffer>`。每次打点都需要执行：

```cpp
if (auto buffer = cache.buffer.lock()) {
    return buffer;
}
```

即使 buffer 是当前线程专用的，`weak_ptr::lock()` 仍要检查对象存活状态、原子增加 strong reference，并在临时 `shared_ptr` 析构时减少引用计数。

### 5.2 当前实现

TLS 直接缓存：

```cpp
struct ThreadLocalBufferCache {
    const ThreadBufferedMetricsCollector* owner{nullptr};
    std::uint64_t ownerGeneration{0};
    ThreadBuffer* buffer{nullptr};
};
```

正常命中只需要比较 owner、generation 和空指针：

```cpp
if (cache.owner == this &&
    cache.ownerGeneration == generation_ &&
    cache.buffer != nullptr) {
    return cache.buffer;
}
```

collector 仍通过 `buffers_` 中的 `shared_ptr<ThreadBuffer>` 持有 buffer，所以 TLS raw pointer 不负责生命周期，只负责快速寻址。

### 5.3 generation 解决什么问题

只比较 collector 地址存在 ABA 风险：

```text
旧 collector 地址为 0x1234，TLS 缓存旧 buffer
旧 collector 被销毁
新 collector 恰好也分配到 0x1234
```

如果仅判断 `cache.owner == this`，新 collector 会误用旧 buffer。每个 collector 创建时取得全局递增的 generation，TLS 必须同时匹配地址和 generation 才能复用缓存。

`buffersMutex_` 只在某个线程第一次注册 buffer 时使用，不属于每次打点的常规路径。

对应代码：

- `ucm/transport/kv/asu/metrics/src/standalone_metrics_backend.cpp`：`ThreadLocalBufferCache`、`NextCollectorGeneration()`、`generation_`、`GetThreadBuffer()` 和 `buffers_`。

## 6. 优化四：保留内置指标批量更新

同一个业务事件通常会同时产生多条指标。例如一次提交可能同时更新 request、entry、error 和 duration。

如果逐条调用 `Add()` 或 `Observe()`，每条指标都要重复执行：

- facade backend 查询；
- backend 虚函数调用；
- TLS buffer 查询；
- `WriteGuard` 进入和退出；
- 动态名称查找。

`UpdateBuiltinBatch()` 把多条更新放在同一个数组中：

```cpp
BuiltinMetricUpdate updates[] = {
    {MetricId::MetricA, valueA},
    {MetricId::MetricB, valueB},
    {MetricId::MetricC, valueC},
};
Metrics::UpdateBuiltinBatch(updates, std::size(updates));
```

collector 只获取一次 buffer，并且整批更新共用一个 `WriteGuard`：

```cpp
auto buffer = GetThreadBuffer();
ThreadBuffer::WriteGuard guard{*buffer};
auto& metrics = buffer->slots[guard.Index()].metrics;

for (...) {
    const auto id = builtinMetricIds_[builtinIndex];
    ApplyUpdate(metrics[id], id, value);
}
```

内置 `MetricId` 通过 `builtinMetricIds_` 直接映射到数组下标，不需要逐条构造字符串或执行哈希查找。

因此 N 条内置指标共同承担：

- 一次 atomic backend load；
- 一次 backend 虚函数调用；
- 一次 TLS buffer 获取；
- 一次双缓冲进入和退出。

对应代码：

- `ucm/transport/kv/asu/metrics/src/metrics.cpp`：facade `UpdateBuiltinBatch()`；
- `ucm/transport/kv/asu/metrics/src/standalone_metrics_backend.cpp`：collector `UpdateBuiltinBatch()`、`builtinMetricIds_` 和 `ApplyUpdate()`；
- `ucm/transport/kv/asu/metrics/include/asu_metrics/metric_names.h`：内置 `MetricId`、名称和类型定义。

## 7. Histogram 的写入和聚合

Histogram 在业务线程中直接完成区间查找和增量累计：

```cpp
metric.sum += value;
++metric.count;
const auto bucket = static_cast<std::size_t>(
    std::lower_bound(buckets.begin(), buckets.end(), value) - buckets.begin());
++metric.bucketCounts[bucket];
```

每个 slot 保存的是 interval bucket count。aggregator 把它们累加到进程级 snapshot；HTTP render 时再将 interval count 转换成 Prometheus 要求的 cumulative bucket，并输出 `_bucket`、`_sum` 和 `_count`。

该设计不会保存所有原始 duration 样本，内存占用由“业务线程数 × 指标数 × bucket 数量”决定，不会随请求总数持续增长。

## 8. 优化后的成本边界

优化后，内置 Histogram 批量打点仍然包含：

- 一次 facade atomic pointer load；
- 一次虚函数调用；
- TLS cache 比较；
- `WriteGuard` 的 atomic load/store；
- 每个 Histogram 的 `lower_bound()`；
- 对线程本地 `MetricState` 的普通内存更新。

因此这里是低开销而不是零开销。指标越多、Histogram bucket 越多，`ApplyUpdate()` 的循环成本仍会增加。

这些优化主要降低 metrics 自身对 CPU 和 cache 的干扰。它们不会直接缩短一个已经在时间戳终点之前结束的被测阶段；判断 metrics 是否污染某段 latency，必须检查该指标的起止时间戳是否包围了 `UpdateBuiltinBatch()`。

## 9. 正确性验证

`DoesNotLoseUpdatesDuringConcurrentFlush` 覆盖最重要的切槽竞争：

- 4 个 writer 线程持续执行批量更新；
- 每个线程更新 Counter 和 Histogram；
- 另一线程持续调用 `Flush()`，强制 aggregator 高频切槽；
- writer 完成后执行最终 flush；
- 精确校验 Counter 和 Histogram `_count` 均等于期望总数。

这个测试验证 writer 进入旧槽与 aggregator 切换旧槽重叠时不会丢点。性能收益仍应在目标 Linux 环境使用 metrics enabled/disabled 和优化前/后的同负载 benchmark 测量，不能只用功能测试推断具体耗时。

对应测试：

- `ucm/transport/kv/asu/test/metrics/standalone_metrics_test.cpp`：`DoesNotLoseUpdatesDuringConcurrentFlush`。

## 10. 代码索引

| 内容 | 文件 |
| --- | --- |
| facade 和 backend 快速指针 | `ucm/transport/kv/asu/metrics/src/metrics.cpp` |
| standalone TLS、双 buffer、聚合与 exporter | `ucm/transport/kv/asu/metrics/src/standalone_metrics_backend.cpp` |
| facade/backend 公共接口 | `ucm/transport/kv/asu/metrics/include/asu_metrics/metrics.h` |
| 内置指标 ID、名称和类型 | `ucm/transport/kv/asu/metrics/include/asu_metrics/metric_names.h` |
| 并发 flush 正确性测试 | `ucm/transport/kv/asu/test/metrics/standalone_metrics_test.cpp` |
| 总体架构与 standalone/UCM 兼容契约 | `docs/source/user-guide/metrics/asu_metrics_architecture_zh.md` |
