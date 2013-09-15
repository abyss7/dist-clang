#pragma once

#include "daemon/thread_pool.h"
#include "net/connection_forward.h"

namespace dist_clang {

namespace net {
class NetworkService;
class Connection;
}

namespace proto {
class Error;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
    bool Initialize(const Configuration& configuration,
                    net::NetworkService& network_service);

  private:
    void HandleNewConnection(bool remote, net::ConnectionPtr connection);
    bool HandleLocalMessage(net::ConnectionPtr connection,
                            const proto::Universal& message,
                            const proto::Error& error);
};

}  // namespace daemon

}  // namespace dist_clang
