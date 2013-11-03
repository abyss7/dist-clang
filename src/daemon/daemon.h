#pragma once

#include "base/attributes.h"
#include "base/queue_aggregator.h"
#include "daemon/file_cache.h"
#include "net/connection_forward.h"

#include <unordered_map>

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace proto {
class Flags;
class Execute;
class Status;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
#if defined(PROFILER)
    Daemon();
#endif  // PROFILER
    ~Daemon();

    bool Initialize(const Configuration& configuration);

  private:
    using ScopedMessage = std::unique_ptr<proto::Universal>;
    using ScopedExecute = std::unique_ptr<proto::Execute>;
    using ScopedTask = std::pair<net::ConnectionPtr, ScopedExecute>;
    using Queue = base::LockedQueue<ScopedTask>;
    using QueueAggregator = base::QueueAggregator<ScopedTask>;
    using CompilerMap =
        std::unordered_map<std::string /* version */, std::string /* path */>;
    using PluginNameMap =
        std::unordered_map<std::string /* name */, std::string /* path */>;
    using PluginMap =
        std::unordered_map<std::string /* version */, PluginNameMap>;

    bool SearchCache(const proto::Execute* message, FileCache::Entry* entry);
    void UpdateCache(const proto::Execute* message,
                     const proto::Status& status);
    bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);
    void HandleNewConnection(net::ConnectionPtr connection);
    bool HandleNewMessage(net::ConnectionPtr connection, ScopedMessage message,
                          const proto::Status& status);

    // Workers
    void DoRemoteExecution(const volatile bool& is_shutting_down,
                           net::fd_t self_pipe, net::EndPointPtr end_point);
    void DoLocalExecution(const volatile bool& is_shutting_down,
                          net::fd_t self_pipe);

    CompilerMap compilers_;
    PluginMap plugins_;
    std::unique_ptr<FileCache> cache_;
    std::unique_ptr<net::NetworkService> network_service_;
    std::unique_ptr<Queue> local_tasks_;
    std::unique_ptr<Queue> failed_tasks_;
    std::unique_ptr<Queue> remote_tasks_;
    std::unique_ptr<QueueAggregator> all_tasks_;
    std::unique_ptr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
