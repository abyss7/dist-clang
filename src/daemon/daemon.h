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
class Error;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
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
        const proto::Error& error);

    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Balancer> balancer_;
    std::unique_ptr<FileCache> cache_;
    std::unordered_map<std::string, std::string> compilers_;
};

}  // namespace daemon
}  // namespace dist_clang
