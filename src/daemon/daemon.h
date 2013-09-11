#pragma once

#include "net/connection_forward.h"

namespace dist_clang {

namespace net {
class NetworkService;
class Connection;
}

namespace proto {
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
    bool Initialize(const Configuration& configuration,
                    net::NetworkService& network_service);

  private:
    void AddConnectionToBalancer(net::ConnectionPtr connection);
    void HandleNewConnection(bool remote, net::ConnectionPtr connection);
    void HandleIncomingMessage(net::ConnectionPtr connection,
                               const proto::Universal& message);
};

}  // namespace daemon

}  // namespace dist_clang
