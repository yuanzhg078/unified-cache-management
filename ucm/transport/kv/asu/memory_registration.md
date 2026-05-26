# ASU Memory Registration

This note describes the current memory registration path from `AsuClient` down
to the HCOMM backend.

## Public API

The client-facing API does not expose memory registration handles.

```cpp
Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                       std::vector<RegisterResult>& results);

Status UnregisterRegions();
```

`RegisterRegions` returns one `RegisterResult` per input region. The result only
contains status:

```cpp
struct RegisterResult {
    Status status;
};
```

`UnregisterRegions` has no input parameters. It unregisters all memory
registrations currently tracked by the client instance.

## Main Idea

The user registers business memory regions:

```text
MemoryRegion {
    memory_type,
    addr,
    size,
    device_id,
    numa_node
}
```

Internally, one user region may need to be registered on many transport
endpoints. HCOMM returns one backend memory handle per endpoint. ASU keeps those
lower-level handles internally and does not return them to the user.

The internal ownership hierarchy is:

```text
AsuClient
  -> AsuTransport MRHandle
      -> endpoint_handle + HCOMM CommMemHandle
```

`MRHandle` is a transport-internal logical ID. `CommMemHandle` is the backend
memory handle returned by HCOMM.

## Initialization Requirement

Memory registration requires endpoint handles, so `AsuClient::Init` must run
first.

During init:

```text
AsuClient::Init
  -> creates one AsuTransport per TransportConfig
  -> AsuTransportImpl::Init
    -> AsuTransportImpl::InitConnections
      -> CreateConnectionManager(HCOMM)
      -> ConnectionManager::Initialize
      -> ConnectionManager::CreateConnection for each endpoint
        -> HcommBackend::CreateConnection
        -> HcommEndpoint::Initialize
        -> HcommEndpoint::CreateChannels
```

`AsuTransportImpl` stores one `EndpointConnection` per configured endpoint:

```cpp
struct EndpointConnection {
    AsuEndpoint endpoint;
    ConnectionEndpointHandle endpoint_handle;
    std::vector<ConnectionHandle> handles;
};
```

`RegisterRegions` uses these existing `endpoint_handle` values. It does not
create endpoints.

## Register Flow

The call chain is:

```text
AsuClientImpl::RegisterRegions
  -> AsuTransportImpl::RegisterRegions
    -> ConnectionManager::RegisterMemory
      -> HcommBackend::RegisterMemory
        -> HcommBackend::RegisterOne
          -> HcommEndpoint::RegisterMemory
            -> HcommProxy::MemReg
```

### Client Layer

`AsuClientImpl::RegisterRegions` iterates over all transports in the current
view:

```text
for each AsuTransport:
    transport->RegisterRegions(regions, transport_results)
```

Transport registration returns `RegisterHandleResult`, which is internal:

```cpp
struct RegisterHandleResult {
    Status status;
    MRHandle handle;
};
```

The client copies only the status into user-facing `RegisterResult`.

When a region succeeds on every transport, the client stores the returned
transport handles:

```text
registered_regions_[asu_id] -> [transport MRHandle, ...]
```

This map is used later by parameterless `AsuClient::UnregisterRegions`.

If one transport fails after another transport already registered the same
region, the client immediately rolls back the successful transport handles for
that region.

### Transport Layer

`AsuTransportImpl::RegisterRegions` receives the original `MemoryRegion` list.
Each region is validated and converted into a connection-manager descriptor:

```cpp
RegisterMemoryDesc {
    memory_type,
    addr,
    size
}
```

The memory type conversion is:

```text
MemoryType::ASCEND_DEVICE -> ConnectionMemType::DEVICE
other memory types       -> ConnectionMemType::HOST
```

For each region, the transport registers the same descriptor on every endpoint
owned by that transport:

```text
for each endpoint_connection:
    ConnectionManager::RegisterMemory(endpoint_handle, {desc}, memory_handles)
```

Each successful endpoint registration produces one `CommMemHandle`. The
transport stores the pair needed to unregister it later:

```cpp
UnregisterMemoryDesc {
    endpoint_handle,
    memory_handle
}
```

When all endpoints succeed for one region, the transport creates one internal
`MRHandle` and stores:

```text
transport registered_regions_[MRHandle] -> {
    MemoryRegion,
    [endpoint_handle + CommMemHandle, ...]
}
```

The transport returns that `MRHandle` only to the client implementation, not to
the public user API.

### ConnectionManager Layer

`ConnectionManager::RegisterMemory` performs common validation before calling
the backend:

- the manager is initialized
- the endpoint handle was created by this manager
- the memory descriptor list is not empty
- every descriptor has non-zero address and size

Then it calls:

```cpp
backend_->RegisterMemory(endpoint_handle, memory_descs, memory_handles);
```

After the backend returns, `ConnectionManager` validates the returned handle
count and checks that no handle is null or duplicated. It also records:

```text
CommMemHandle -> endpoint_handle
```

That mapping is used to validate unregister requests.

### HCOMM Backend Layer

`HcommBackend::RegisterMemory` resolves the `ConnectionEndpointHandle` to a
`HcommEndpoint`.

For each `RegisterMemoryDesc`, `HcommBackend::RegisterOne` builds an HCOMM
`CommMem`:

```cpp
CommMem mem{};
mem.type = desc.memory_type == ConnectionMemType::HOST
               ? COMM_MEM_TYPE_HOST
               : COMM_MEM_TYPE_DEVICE;
mem.addr = reinterpret_cast<void*>(desc.addr);
mem.size = desc.size;
```

Then it calls:

```cpp
endpoint.RegisterMemory(mem, memory_handle);
```

`HcommEndpoint::RegisterMemory` is the final wrapper before HCOMM:

```cpp
HcommProxy::MemReg(endpoint_handle_, nullptr, &mem, &hcomm_mem_handle);
```

The returned `hcomm_mem_handle` is stored as a `CommMemHandle`.

## Unregister Flow

The user calls:

```cpp
client->UnregisterRegions();
```

The call chain is:

```text
AsuClientImpl::UnregisterRegions
  -> AsuTransportImpl::UnregisterRegions(transport MRHandle list)
    -> ConnectionManager::UnregisterMemory
      -> HcommBackend::UnregisterMemory
        -> HcommEndpoint::UnregisterMemory
          -> HcommProxy::MemUnreg
```

The client swaps out its internal map:

```text
AsuId -> [transport MRHandle, ...]
```

Then it calls each transport with the handles that belong to that transport.

The transport resolves each `MRHandle` to its stored unregister descriptors:

```text
MRHandle -> [endpoint_handle + CommMemHandle, ...]
```

It passes all descriptors to `ConnectionManager::UnregisterMemory`. The
connection manager validates that each memory handle belongs to the supplied
endpoint, then calls the HCOMM backend. The HCOMM endpoint finally calls:

```cpp
HcommProxy::MemUnreg(endpoint_handle_, memory_handle);
```

## Example

Suppose the user registers one region:

```text
R = addr 0x100000, size 4096
```

The client has two ASU transports:

```text
ASU-0
ASU-1
```

Each transport has two endpoints:

```text
ASU-0 endpoint-0
ASU-0 endpoint-1
ASU-1 endpoint-0
ASU-1 endpoint-1
```

Registering `R` creates backend memory handles on all endpoints:

```text
ASU-0 endpoint-0 -> CommMemHandle a
ASU-0 endpoint-1 -> CommMemHandle b
ASU-1 endpoint-0 -> CommMemHandle c
ASU-1 endpoint-1 -> CommMemHandle d
```

Each transport groups its endpoint handles behind one internal `MRHandle`:

```text
ASU-0 transport MRHandle 10 -> {a, b}
ASU-1 transport MRHandle 20 -> {c, d}
```

The client stores:

```text
registered_regions_[ASU-0] -> [10]
registered_regions_[ASU-1] -> [20]
```

The user only sees whether `R` registered successfully. On
`UnregisterRegions()`, the client uses the stored map to unregister everything.

## Failure Handling

Registration is all-or-cleanup per region at the client layer:

- if a transport fails for one region, that region is marked failed
- transport handles already created for that region are immediately unregistered
- successful regions remain tracked by the client
- the overall return status is `PARTIAL_FAILED` if any region fails

Transport registration also performs endpoint-level cleanup. If registering a
region succeeds on one endpoint but fails on a later endpoint in the same
transport, the transport unregisters the endpoint handles it already created for
that region.

## Handle Visibility

The public API does not expose these handles:

- `MRHandle`
- `CommMemHandle`
- `ConnectionEndpointHandle`
- `ConnectionHandle`

Their roles are internal:

```text
MRHandle                 transport registration lookup key
CommMemHandle            HCOMM memory registration handle
ConnectionEndpointHandle connection-manager endpoint lookup key
ConnectionHandle         channel/connection lookup key
```

