#pragma once

#include "base/attributes.h"
#include "base/thread_pool.h"
#include "daemon/balancer.h"
#include "daemon/file_cache.h"
#include "net/connection_forward.h"

#include <unordered_map>

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace proto {
class Flags;
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

  public:
    using ScopedMessage = ::std::unique_ptr<proto::Universal>;

    Daemon();
    ~Daemon();

    bool Initialize(
        const Configuration& configuration,
        net::NetworkService& network_service);
    bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);

    inline base::ThreadPool* WEAK_PTR pool();
    inline Balancer* WEAK_PTR balancer();
    inline FileCache* WEAK_PTR cache();

  private:
    // Invoked on a new connection.
    void HandleNewConnection(
        net::ConnectionPtr connection);

    // Invoked on a first message from a new connection.
    bool HandleNewMessage(
        net::ConnectionPtr connection,
        ScopedMessage message,
        const proto::Status& status);

    std::unique_ptr<base::ThreadPool> pool_;
    std::unique_ptr<Balancer> balancer_;
    std::unique_ptr<FileCache> cache_;
    CompilerMap compilers_;
    PluginMap plugins_;
};

base::ThreadPool* Daemon::pool() {
  return pool_.get();
}

Balancer* Daemon::balancer() {
  return balancer_.get();
}

FileCache* Daemon::cache() {
  return cache_.get();
}

}  // namespace daemon
}  // namespace dist_clang
