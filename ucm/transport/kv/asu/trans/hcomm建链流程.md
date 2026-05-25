# HCOMM 建链调用关系

本文记录当前 UCM ASU transport 中 HCOMM backend 的建链流程。当前实现采用配置驱动的 endpoint 描述，并在一个 HCOMM Endpoint 上批量创建多个 Channel。

## 代码分层

```mermaid
flowchart TB
    App[调用方 / ASU Transport] --> CM[ConnectionManager]
    CM --> BackendIface[ConnectionBackend 接口]
    BackendIface --> HcommBackend[HcommBackend]
    HcommBackend --> HcommEndpoint[HcommEndpoint]
    HcommEndpoint --> HcommProxy[HcommProxy]
    HcommProxy --> HcommAPI[HCOMM C API]

    HcommBackend -.配置解析.-> EndpointDesc[EndpointDesc local/remote]
    HcommEndpoint -.资源持有.-> EndpointHandle[EndpointHandle]
    HcommEndpoint -.资源持有.-> ThreadHandle[ThreadHandle]
    HcommEndpoint -.资源持有.-> ChannelHandles[ChannelHandle 列表]
    HcommEndpoint -.资源持有.-> MemHandles[MemHandle 列表]
```

| 层级 | 职责 |
| --- | --- |
| `ConnectionManager` | 对外统一连接管理接口，维护 manager 级别的 connection/memory handle 集合。 |
| `ConnectionBackend` | 内部 backend 抽象，屏蔽 HCOMM/AIV 等实现差异。 |
| `HcommBackend` | 解析 endpoint 配置，适配 `ConnectionBackend` 接口，维护 `ConnectionHandle -> HcommEndpoint + ChannelHandle` 映射。 |
| `HcommEndpoint` | 封装 HCOMM endpoint/channel/memory 原语，持有 `EndpointHandle`、`ThreadHandle`、channel 和 memory 集合。 |
| `HcommProxy` | weak symbol 方式调用 HCOMM C API。 |

## 初始化流程

```mermaid
sequenceDiagram
    participant User as 调用方
    participant CM as ConnectionManager
    participant HB as HcommBackend

    User->>CM: Initialize()
    CM->>HB: Initialize(config)
    HB->>HB: 保存 config_
    HB->>HB: LoadEndpointConfigFile()
    HB->>HB: 读取 tc/sl/send_size/flag_size/remote addr
    HB-->>CM: Status::OK
    CM-->>User: Status::OK
```

配置文件通过 `attrs["endpoint_config_file"]` 指定。文件内支持 `key=value` 或 `key: value`，支持 `#` / `;` 注释。配置 key 会归一化：

- `local.protocol` -> `local_protocol`
- `local-comm-id` -> `local_comm_id`

显式传入的 `config.attrs` 优先级高于配置文件中的同名字段。

## 建链流程

```mermaid
sequenceDiagram
    participant User as 调用方
    participant CM as ConnectionManager
    participant HB as HcommBackend
    participant EP as HcommEndpoint
    participant Proxy as HcommProxy
    participant HCOMM as HCOMM API

    User->>CM: CreateConnection(local_ip, remote_ip, port, qp_num, timeout)
    CM->>CM: 校验 initialized / qp_num / timeout
    CM->>HB: CreateConnection(...)

    HB->>HB: BuildEndpoint(local, "local")
    HB->>HB: BuildEndpoint(remote, "remote")
    HB->>EP: new HcommEndpoint(local_endpoint, remote_endpoint)

    HB->>EP: Initialize()
    EP->>Proxy: EndpointCreate(local_endpoint)
    Proxy->>HCOMM: HcommEndpointCreate(...)
    HCOMM-->>Proxy: EndpointHandle
    Proxy-->>EP: EndpointHandle
    EP->>Proxy: ThreadAlloc(engine, 1, notify_num)
    Proxy->>HCOMM: HcommThreadAlloc(...)
    HCOMM-->>Proxy: ThreadHandle
    Proxy-->>EP: ThreadHandle

    HB->>EP: CreateChannels(port, qp_num, tc, sl, hccs_qos)
    EP->>EP: HcommChannelDescInit(qp_num)
    EP->>EP: 填 role / remoteEndpoint / port / protocol attrs / channel_index
    EP->>Proxy: ChannelCreate(endpoint, engine, descs, qp_num)
    Proxy->>HCOMM: HcommChannelCreate(...)
    HCOMM-->>Proxy: ChannelHandle[qp_num]
    Proxy-->>EP: ChannelHandle[qp_num]
    EP-->>HB: ChannelHandle[qp_num]

    HB->>HB: 为每个 ChannelHandle 创建 HcommConnection
    HB->>HB: connections_[channel] = {shared HcommEndpoint, channel}
    HB-->>CM: connection_handles
    CM->>CM: 登记 connection_handles_
    CM-->>User: connection_handles
```

## EndpointDesc 构造

`HcommBackend::BuildEndpoint()` 会按 `local_` / `remote_` 前缀从 attrs 中读取 endpoint 信息；如果未配置，则 RoCE/UBOE 地址仍可使用 `CreateConnection()` 入参中的 `local_ip` / `remote_ip` 作为默认值。

```mermaid
flowchart LR
    Build[BuildEndpoint(prefix)] --> Protocol[FillEndpointProtocol]
    Build --> Address[FillEndpointAddress]
    Build --> Location[FillEndpointLocation]

    Protocol --> RoCE[roce]
    Protocol --> UBOE[uboe]
    Protocol --> HCCS[hccs]
    Protocol --> UB[ub_ctp / ub_tp]

    Address --> IP[RoCE/UBOE: IPv4 或 IPv6]
    Address --> ID[HCCS: COMM_ADDR_TYPE_ID]
    Address --> EID[UB: COMM_ADDR_TYPE_EID]

    Location --> Host[host: loc.host.id]
    Location --> Device[device: devPhyId/superDevId/superPodIdx/serverIdx]
```

常用配置示例：

```ini
protocol = uboe
placement = device
port = 6000

local.comm_id = 192.168.1.10
remote.comm_id = 192.168.1.11

local.phy_device_id = 0
remote.phy_device_id = 1

tc = 0
sl = 0
send_size = 4096
flag_size = 8
remote_send_addr = 0x100000000
remote_flag_addr = 0x200000000
```

## 资源归属

```mermaid
flowchart TB
    EP[shared_ptr<HcommEndpoint>] --> EPH[EndpointHandle]
    EP --> TH[ThreadHandle]
    EP --> CHS[channels_ set]
    EP --> MEMS[memory_handles_ set]

    C0[ConnectionHandle 0] --> HC0[HcommConnection]
    C1[ConnectionHandle 1] --> HC1[HcommConnection]
    C2[ConnectionHandle 2] --> HC2[HcommConnection]

    HC0 --> EP
    HC1 --> EP
    HC2 --> EP

    HC0 --> CH0[ChannelHandle 0]
    HC1 --> CH1[ChannelHandle 1]
    HC2 --> CH2[ChannelHandle 2]
```

当前模型是：

- 一次 `CreateConnection(..., qp_num, ...)` 创建一个 `HcommEndpoint`。
- 该 endpoint 内部持有一个 `EndpointHandle` 和一个 `ThreadHandle`。
- 一次 `ChannelCreate(..., qp_num, ...)` 在同一 endpoint 上创建 `qp_num` 个 channel。
- 返回给调用方的 `ConnectionHandle` 实际上等于对应的 `ChannelHandle`。
- 每个 `HcommConnection` 共享同一个 `HcommEndpoint`，但绑定不同的 `ChannelHandle`。

## 发送流程

```mermaid
sequenceDiagram
    participant User as 调用方
    participant CM as ConnectionManager
    participant HB as HcommBackend
    participant EP as HcommEndpoint
    participant Proxy as HcommProxy
    participant HCOMM as HCOMM API

    User->>CM: Send(io_batches, kernel_count, quiet_count)
    CM->>CM: 校验 connection_handle / buffer
    CM->>HB: Send(...)
    loop 每个 io_batch
        HB->>HB: connections_[connection_handle]
        HB->>EP: GetThreadHandle()
        HB->>Proxy: WriteNbiOnThread(thread, channel, remote_send, send_buffer)
        Proxy->>HCOMM: HcommWriteNbiOnThread(...)
        HB->>Proxy: WriteNbiOnThread(thread, channel, remote_flag, flag_buffer)
        Proxy->>HCOMM: HcommWriteNbiOnThread(...)
        alt 到达 quiet_count
            HB->>Proxy: ChannelFenceOnThread(thread, channel)
            Proxy->>HCOMM: HcommChannelFenceOnThread(...)
        end
    end
```

## 内存注册流程

```mermaid
sequenceDiagram
    participant User as 调用方
    participant CM as ConnectionManager
    participant HB as HcommBackend
    participant EP as HcommEndpoint
    participant Proxy as HcommProxy
    participant HCOMM as HCOMM API

    User->>CM: RegisterMemory(connection_handle, memory_descs)
    CM->>CM: 校验 connection_handle / memory desc
    CM->>HB: RegisterMemory(...)
    HB->>HB: 找到 connection 对应的 HcommEndpoint
    HB->>EP: RegisterMemory(CommMem)
    EP->>Proxy: MemReg(endpoint_handle, mem)
    Proxy->>HCOMM: HcommMemReg(...)
    HCOMM-->>Proxy: HcommMemHandle
    Proxy-->>EP: HcommMemHandle
    EP->>EP: memory_handles_.insert(handle)
    EP-->>HB: handle
    HB-->>CM: memory_handles
    CM->>CM: 登记 memory_handles_
    CM-->>User: memory_handles
```

## 删除流程

```mermaid
sequenceDiagram
    participant User as 调用方
    participant CM as ConnectionManager
    participant HB as HcommBackend
    participant EP as HcommEndpoint
    participant Proxy as HcommProxy
    participant HCOMM as HCOMM API

    User->>CM: DeleteConnections(connection_handles)
    CM->>HB: DeleteConnections(...)
    loop 每个 connection_handle
        HB->>HB: connections_.erase(handle)
        HB->>EP: DestroyChannel(channel_handle)
        EP->>Proxy: ChannelDestroy(channel)
        Proxy->>HCOMM: HcommChannelDestroy(...)
        alt 最后一个 HcommEndpoint 引用
            HB->>EP: Finalize()
            EP->>Proxy: MemUnreg(残留 memory)
            EP->>Proxy: ThreadFree(thread)
            EP->>Proxy: EndpointDestroy(endpoint)
        end
    end
```

## 与 HIXL 的对应关系

| UCM 当前文件 | HIXL 近似对应 | 说明 |
| --- | --- | --- |
| `hcomm_endpoint.{h,cpp}` | `cs/endpoint.{h,cc}` + `cs/channel.{h,cc}` | 封装 endpoint、channel、mem 的 HCOMM 原语。 |
| `hcomm_backend.{h,cpp}` | `cs/hixl_cs_client.cc` 中 client 侧资源编排的一部分 | UCM 暂无 TCP 协商，因此这里只做配置驱动的 endpoint 构造和本地建链。 |
| 暂无 | `cs/endpoint_store.{h,cc}` | 等需要 server 侧 endpoint match / 多 endpoint 管理时再补。 |
| 暂无 | `conn_msg_handler` / `mem_msg_handler` | 当前没有 HIXL 的 TCP 控制面和 MemExport/MemImport 协商。 |

