#pragma once

#include "net/connection_forward.h"

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace daemon {

class Balancer {
  public:
    explicit Balancer(net::NetworkService& network_service);

    net::ConnectionPtr Decide();
};

}  // namespace daemon
}  // namespace dist_clang
