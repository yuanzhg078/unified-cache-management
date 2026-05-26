# ConnectionManager & HcommBackend 实现架构

## 整体分层

```
AsuTransportImpl          ← 上层传输入口，持有 ConnectionManager
  └─ ConnectionManager    ← 公开门面类（Pimpl 模式，只转发）
       └─ Impl            ← 校验层 + 状态追踪层（藏在 .cpp 内部，无独立头文件）
            └─ ConnectionBackend   ← 抽象后端接口（纯虚类）
                 ├─ HcommBackend   ← 基于 hcomm 库的具体后端
                 └─ AivBackend     ← 基于 AIV 的具体后端
```

## 各层职责

| 层 | 角色 | 干什么 | 不干什么 |
|---|---|---|---|
| **ConnectionManager** | 门面 | 暴露公开 API，转发给 Impl | 不做任何业务逻辑 |
| **Impl** | 校验+追踪 | 参数校验、handle 状态追踪、线程安全 | 不做底层建链/传输 |
| **ConnectionBackend** | 抽象接口 | 定义"后端该有哪些方法" | 不做任何实现 |
| **HcommBackend / AivBackend** | 执行者 | 真正调用底层库建链、传输、内存注册 | — |

## 调用链示例：CreateConnection

```
connection_manager.CreateConnection(request, handles)
  │
  第1步 ─ ConnectionManager（门面，只转发）
  → impl_->CreateConnection(request, handles)
  │
  第2步 ─ Impl（校验+追踪）
  → 检查 initialized_?
  → 检查 qp_num > 0? timeout_ms > 0?
  → 通过后调用 backend_->CreateConnection(...)
  → 拿到结果后，记录 handle 到映射表
  │
  第3步 ─ ConnectionBackend（虚函数调用）
  → 实际跑到 HcommBackend::CreateConnection()
  │
  第4步 ─ HcommBackend（真正干活）
  → 创建 HcommEndpoint，调 hcomm 库建链
  → 创建 Channel，返回结果给 Impl
```

---

## ConnectionManager（公开类）

- **头文件**: `include/asu_transport/connection_manager.h`
- **源文件**: `src/connection_manager.cpp`
- **设计模式**: Pimpl（Pointer to Implementation），`class Impl;` 前向声明在头文件，`class ConnectionManager::Impl` 的完整定义藏在 .cpp 中
- **成员**: 只有一个 `std::unique_ptr<Impl> impl_`
- **所有方法**: 都是一行转发 `return impl_->xxx()`

### 关键类型定义

| 类型 | 定义 | 说明 |
|------|------|------|
| `ConnectionHandle` | `std::uint64_t` | 单个连接句柄，对应一个 QP/Channel |
| `ConnectionEndpointHandle` | `std::uint64_t` | Endpoint 句柄，一组连接的容器 |
| `CommMemHandle` | `void*` | 内存注册句柄 |

---

## ConnectionManager::Impl（内部实现类）

- **定义位置**: `src/connection_manager.cpp` 内部，**无独立头文件**
- **语法**: `class ConnectionManager::Impl` — C++ 嵌套类（nested class），`::` 表示 Impl 属于 ConnectionManager

### 重要成员变量

| 成员 | 类型 | 作用 |
|------|------|------|
| `config_` | `ConnectionManagerConfig` | 配置信息，含 `backend_type`（HCOMM/AIV）和 `attrs`（KV 属性字典） |
| `backend_` | `std::unique_ptr<ConnectionBackend>` | 核心：通过工厂方法 `CreateConnectionBackend(config_.backend_type)` 创建 |
| `initialized_` | `bool` | 初始化状态标记 |
| `endpoint_handles_` | `std::unordered_set<ConnectionEndpointHandle>` | 已创建的 endpoint handle 集合（校验合法性） |
| `connection_handles_` | `std::unordered_set<ConnectionHandle>` | 已创建的 connection handle 集合（校验合法性） |
| `connection_endpoint_handles_` | `std::unordered_map<ConnectionHandle, ConnectionEndpointHandle>` | connection → endpoint 映射 |
| `memory_handles_` | `std::unordered_map<CommMemHandle, ConnectionEndpointHandle>` | 内存句柄 → endpoint 映射 |
| `lifecycle_mu_` | `std::shared_mutex` | 生命周期读写锁：Initialize/Finalize 用独占锁，Send 用共享锁 |
| `mu_` | `std::mutex` | 状态数据互斥锁 |

### 各方法与 backend 的对应关系

| Impl 方法 | 委托给 | Impl 额外做的事 |
|---|---|---|
| `Initialize()` | `backend_->Initialize(config_)` | 设置 `initialized_` 标记 |
| `Finalize()` | `backend_->Finalize()` | 清空所有 handle 集合 + 映射表 |
| `CreateConnection()` | `backend_->CreateConnection()` | 校验参数 + 校验返回值合法性 + 记录 handle 映射 |
| `DeleteConnections()` | `backend_->DeleteConnections()` | 校验 handle 存在性 + 删除成功后清理映射 + 自动清理空 endpoint |
| `Send()` | `backend_->Send()` | 校验 connection_handle 存在性 + send_buffer/flag_buffer 非空 |
| `RegisterMemory()` | `backend_->RegisterMemory()` | 校验 endpoint 存在性 + addr/size 合法性 + 记录 mem_handle → endpoint |
| `UnregisterMemory()` | `backend_->UnregisterMemory()` | 校验 mem_handle 属于该 endpoint + 清理映射 |

---

## ConnectionBackend（抽象接口）

- **头文件**: `src/connection_backend.h`
- **源文件**: `src/connection_backend.cpp`（只含工厂方法）

### 纯虚接口

| 虚方法 | 作用 |
|--------|------|
| `Initialize(config)` | 初始化后端 |
| `Finalize()` | 清理后端 |
| `CreateConnection(request, endpoint_handle, connection_handles)` | 创建连接 |
| `DeleteConnections(connection_handles)` | 批量删除连接 |
| `Send(io_batches, kernel_count, quiet_count)` | 批量发送 |
| `RegisterMemory(endpoint_handle, memory_descs, memory_handles)` | 注册内存 |
| `UnregisterMemory(memory_descs)` | 批量注销内存 |

### 工厂方法

```cpp
std::unique_ptr<ConnectionBackend> CreateConnectionBackend(ConnectionBackendType type) {
    if (type == ConnectionBackendType::AIV) return std::make_unique<AivBackend>();
    return std::make_unique<HcommBackend>();
}
```

---

## HcommBackend（具体后端实现）

- **头文件**: `src/hcomm_backend.h`
- **源文件**: `src/hcomm_backend.cpp`

### 内部数据结构

#### HcommConnection — 单个连接的状态

| 成员 | 类型 | 作用 |
|------|------|------|
| `endpoint` | `std::shared_ptr<HcommEndpoint>` | 该连接所属的 endpoint 对象 |
| `io_mu` | `std::shared_ptr<std::mutex>` | 该连接专属的 IO 互斥锁 |
| `channel_handle` | `ChannelHandle` | hcomm 通道句柄（底层 QP） |
| `send_size` | `std::uint64_t` | 数据发送大小 |
| `flag_size` | `std::uint64_t` | 标志位大小（默认 1） |
| `remote_send_addr` | `void*` | 对端发送缓冲区虚拟地址 |
| `remote_flag_addr` | `void*` | 对端标志位虚拟地址 |
| `owns_channel` | `bool` | 是否由本端创建 channel（client 侧为 true） |
| `control_fd` | `int` | TCP 控制面 socket fd |

#### ImportedRemoteMemory — 导入的远端内存

| 成员 | 类型 | 作用 |
|------|------|------|
| `tag` | `std::string` | 内存标签 |
| `memory` | `CommMem` | hcomm 通用内存描述（type/addr/size） |
| `export_desc` | `std::vector<std::uint8_t>` | 远端内存导出描述序列化数据 |

### 重要成员变量

| 成员 | 类型 | 作用 |
|------|------|------|
| `config_` | `ConnectionManagerConfig` | 配置信息 |
| `active_attrs_` | `const unordered_map<string,string>*` | 当前生效的属性字典指针 |
| `initialized_` | `bool` | 初始化状态 |
| `tc_` | `std::uint8_t` | RDMA Traffic Class（QoS） |
| `sl_` | `std::uint8_t` | RDMA Service Level（QoS） |
| `default_send_size_` | `std::uint64_t` | 默认发送大小 |
| `default_flag_size_` | `std::uint64_t` | 默认标志位大小 |
| `default_remote_send_addr_` | `void*` | 默认远端发送地址 |
| `default_remote_flag_addr_` | `void*` | 默认远端标志位地址 |
| `next_endpoint_handle_` | `ConnectionEndpointHandle` | endpoint handle 自增分配器（从 1 开始） |
| **`endpoints_`** | `unordered_map<ConnectionEndpointHandle, shared_ptr<HcommEndpoint>>` | endpoint handle → HcommEndpoint 映射 |
| **`endpoint_sockets_`** | `unordered_map<ConnectionEndpointHandle, vector<int>>` | 每个 endpoint 的 TCP 控制面 socket 列表 |
| **`remote_endpoint_handles_`** | `unordered_map<ConnectionEndpointHandle, uint64_t>` | 本端 endpoint handle → 对端 endpoint handle |
| **`imported_remote_mems_`** | `unordered_map<ConnectionEndpointHandle, vector<ImportedRemoteMemory>>` | 每个 endpoint 导入的远端内存 |
| **`connections_`** | `unordered_map<ConnectionHandle, HcommConnection>` | connection handle → HcommConnection 映射 |
| `mu_` | `std::mutex` | 全局互斥锁 |

### 建链核心流程

```
CreateConnection()
  ├─ CreateConnectionsOnEndpoint()        // 在已有 endpoint 上创建连接
  ├─ CreateClientServerConnection()       // 完整建链流程
  │     ├─ BuildEndpoint()                // 构造 EndpointDesc
  │     │    ├─ FillEndpointProtocol()    // 从 attrs 提取协议(roce/hccs/uboe)
  │     │    ├─ FillEndpointAddress()     // 从 attrs 提取 IP/端口
  │     │    └─ FillEndpointLocation()    // 从 attrs 提取设备定位信息
  │     ├─ HcommEndpoint::Initialize()   // 创建 hcomm endpoint + thread
  │     ├─ TCP 握手交换远端信息            // socket connect/accept
  │     ├─ ImportRemoteMemories()         // 通过 socket 交换并 import 远端内存
  │     └─ HcommEndpoint::CreateChannels() // 创建 hcomm channel (QP)
  └─ (server 侧): accept + 交换信息 + create channel
```

---

## HcommEndpoint（hcomm 端点封装）

- **头文件**: `src/hcomm_endpoint.h`

### 重要成员变量

| 成员 | 类型 | 作用 |
|------|------|------|
| `local_endpoint_` | `EndpointDesc` | 本端 endpoint 描述（协议、地址、定位） |
| `remote_endpoint_` | `EndpointDesc` | 对端 endpoint 描述 |
| `endpoint_handle_` | `EndpointHandle` | hcomm 库返回的 endpoint 句柄（void*） |
| `thread_handle_` | `ThreadHandle` | hcomm 线程句柄（传输操作执行线程） |
| `initialized_` | `bool` | 初始化状态 |
| `owns_endpoint_` | `bool` | 是否拥有 endpoint（Finalize 时是否销毁） |
| `owns_thread_` | `bool` | 是否拥有 thread（Finalize 时是否释放） |
| `channels_` | `std::unordered_set<ChannelHandle>` | 该 endpoint 下所有 channel |
| `memory_handles_` | `std::unordered_set<CommMemHandle>` | 该 endpoint 上注册的内存句柄 |
| `mu_` | `std::mutex` | endpoint 内部互斥锁 |

---

## 关键数据流转关系

```
ConnectionHandle ──映射──> HcommConnection
                         ├─ endpoint ──> HcommEndpoint (含 endpoint_handle_, thread_handle_, channels_)
                         ├─ channel_handle (QP)
                         ├─ remote_send_addr / remote_flag_addr
                         └─ control_fd (TCP socket)

ConnectionEndpointHandle ──映射──> HcommEndpoint
                                 ├─ endpoint_handle_ (hcomm 库层)
                                 ├─ thread_handle_
                                 ├─ channels_
                                 └─ memory_handles_

ConnectionEndpointHandle ──映射──> imported_remote_mems_ (远端导入内存)
ConnectionEndpointHandle ──映射──> endpoint_sockets_ (TCP 控制面连接)
ConnectionEndpointHandle ──映射──> remote_endpoint_handles_ (对端 handle)
```

---

## AsuTransportImpl 与 ConnectionManager 的关系

| 成员 | 类型 | 作用 |
|------|------|------|
| `connection_manager_` | `std::unique_ptr<ConnectionManager>` | 所有建链/传输操作通过此门面完成 |
| `endpoint_connections_` | `std::vector<EndpointConnection>` | 预建的 endpoint+connection 列表 |

`EndpointConnection` 结构体：
```cpp
struct EndpointConnection {
    AsuEndpoint endpoint;
    ConnectionEndpointHandle endpoint_handle;
    std::vector<ConnectionHandle> handles;
};
```

---

## 设计模式总结

1. **Pimpl 模式**: ConnectionManager 对外只暴露 `class Impl;` 前向声明，Impl 的完整定义藏在 .cpp 中，降低编译依赖，隐藏内部细节
2. **策略模式 (Strategy)**: ConnectionBackend 是抽象接口，HcommBackend/AivBackend 是具体策略，通过 `ConnectionBackendType` 选择
3. **工厂方法**: `CreateConnectionBackend(type)` 根据类型创建对应后端，调用方只和抽象接口打交道
4. **嵌套类**: `class ConnectionManager::Impl` 用 `::` 表示 Impl 属于 ConnectionManager，语义清晰