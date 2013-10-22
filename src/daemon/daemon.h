#pragma once

#include "base/attributes.h"
#include "base/locked_queue.h"
#include "base/worker_pool.h"
#include "daemon/file_cache.h"
#include "net/connection_forward.h"

#include <google/protobuf/message.h>
#include <unordered_map>

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace proto {
class Flags;
class Host;
class Status;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
    // The version is a key, and the compiler's path is a value.
    using CompilerMap = std::unordered_map<std::string, std::string>;

    // The name is a key, and the plugin's path is a value.
    using PluginNameMap = std::unordered_map<std::string, std::string>;

    // The version is a key.
    using PluginMap = std::unordered_map<std::string, PluginNameMap>;

    using ScopedMessage = std::unique_ptr<::google::protobuf::Message>;
    using TaskQueue = base::LockedQueue<ScopedMessage>;

  public:
    Daemon();
    ~Daemon();

    bool Initialize(
        const Configuration& configuration,
        net::NetworkService& network_service);
    bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);

  private:
    // Invoked on a new connection.
    void HandleNewConnection(
        net::ConnectionPtr connection);

    // Invoked on a first message from a new connection.
    bool HandleNewMessage(
        net::ConnectionPtr connection,
        const proto::Universal& message,
        const proto::Status& status);

    void LocalCompilation(const volatile bool& should_close, net::fd_t pipe);
    void RemoteCompilation(const volatile bool& should_close, net::fd_t pipe,
                           const proto::Host& remote);

    std::unique_ptr<base::WorkerPool> pool_;
    std::unique_ptr<TaskQueue> tasks_;
    std::unique_ptr<TaskQueue> local_only_tasks_;
    std::unique_ptr<FileCache> cache_;
    CompilerMap compilers_;
    PluginMap plugins_;
};

}  // namespace daemon
}  // namespace dist_clang
