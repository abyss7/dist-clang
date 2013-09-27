#pragma once

#include "net/connection_forward.h"
#include "proto/config.pb.h"

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace daemon {

class Balancer {
  public:
    explicit Balancer(net::NetworkService& network_service);

    void AddRemote(const proto::Host& remote);
    net::ConnectionPtr Decide();

  private:
    net::NetworkService& service_;
    std::vector<proto::Host> remotes_;
};

}  // namespace daemon
}  // namespace dist_clang
