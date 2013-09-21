#include "daemon/balancer.h"

#include "net/connection.h"
#include "net/network_service.h"

namespace dist_clang {
namespace daemon {

Balancer::Balancer(net::NetworkService &network_service) {
  // TODO: implement this.
}

net::ConnectionPtr Balancer::Decide() {
  // TODO: implement this.
  return net::ConnectionPtr();
}

}  // namespace daemon
}  // namespace dist_clang
