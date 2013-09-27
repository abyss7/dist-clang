#pragma once

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
class Status;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
    // The version is a key, and the compiler's path is a value.
    using CompilerMap = std::unordered_map<std::string, std::string>;

    bool Initialize(
        const Configuration& configuration,
        net::NetworkService& network_service);

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

}  // namespace daemon
}  // namespace dist_clang
