#pragma once

#include "base/attributes.h"
#include "daemon/balancer.h"
#include "daemon/file_cache.h"
#include "daemon/thread_pool.h"
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

  public:
    bool Initialize(
        const Configuration& configuration,
        net::NetworkService& network_service);
    bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);

    inline Balancer* WEAK_PTR balancer();
    inline FileCache* WEAK_PTR cache();

  private:
    // Invoked on a new connection.
    void HandleNewConnection(
        net::ConnectionPtr connection);

    // Invoked on a first message from a new connection.
    bool HandleNewMessage(
        net::ConnectionPtr connection,
        const proto::Universal& message,
        const proto::Status& status);

    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Balancer> balancer_;
    std::unique_ptr<FileCache> cache_;
    CompilerMap compilers_;
};

Balancer* WEAK_PTR Daemon::balancer() {
  return balancer_.get();
}

FileCache* WEAK_PTR Daemon::cache() {
  return cache_.get();
}

}  // namespace daemon
}  // namespace dist_clang
