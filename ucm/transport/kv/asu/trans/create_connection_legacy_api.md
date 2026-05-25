# Legacy CreateConnection API

The connection creation API was replaced by `CreateConnectionRequest` to avoid
growing fixed parameter lists. The previous public `ConnectionManager` overloads
were:

```cpp
Status CreateConnection(const std::string& local_ip, const std::string& remote_ip,
                        std::uint32_t port, std::uint32_t qp_num,
                        std::uint32_t timeout_ms,
                        std::vector<ConnectionHandle>& connection_handles);

Status CreateConnection(const std::string& local_ip, const std::string& remote_ip,
                        std::uint32_t port, std::uint32_t qp_num,
                        std::uint32_t timeout_ms,
                        const std::unordered_map<std::string, std::string>& attrs,
                        std::vector<ConnectionHandle>& connection_handles);
```

The previous backend virtual interface was:

```cpp
virtual Status CreateConnection(
    const std::string& local_ip, const std::string& remote_ip,
    std::uint32_t port, std::uint32_t qp_num, std::uint32_t timeout_ms,
    const std::unordered_map<std::string, std::string>& attrs,
    std::vector<ConnectionHandle>& connection_handles) = 0;
```
