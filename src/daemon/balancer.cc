#include "daemon/balancer.h"

#include "base/random.h"
#include "net/connection.h"
#include "net/network_service.h"

#include <iostream>
#include <string>

namespace dist_clang {
namespace daemon {

Balancer::Balancer(net::NetworkService &network_service)
  : service_(network_service) {}

void Balancer::AddRemote(const proto::Host &remote) {
  // TODO: check for duplicates.
  remotes_.push_back(remote);
}

net::ConnectionPtr Balancer::Decide() {
  // TODO: implement this.
  const proto::Host& remote =
      remotes_[base::Random<size_t>() % remotes_.size()];
  std::string error;
  auto connection = service_.Connect(remote.host(), remote.port(), &error);
  if (!connection)
    std::cerr << "Failed to connect to " << remote.host() << ":"
              << remote.port() << " : " << error << std::endl;
  return connection;
}

}  // namespace daemon
}  // namespace dist_clang
