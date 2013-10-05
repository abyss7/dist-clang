#include "daemon/balancer.h"

#include "base/random.h"
#include "net/connection.h"
#include "net/network_service.h"

#include <iostream>
#include <string>

namespace dist_clang {
namespace daemon {

std::atomic<size_t> Balancer::index_(0);

Balancer::Balancer(net::NetworkService& network_service)
  : service_(network_service) {
}

void Balancer::AddRemote(const proto::Host& remote) {
  remotes_.insert(remote);
}

bool Balancer::Decide(const ConnectCallback& callback, std::string* error) {
  auto remote_index =
      std::atomic_fetch_add(&index_, 1ul) % (remotes_.size() + 1);
  if (remote_index == remotes_.size()) {
    return false;
  }

  auto remote = remotes_.cbegin();
  std::advance(remote, remote_index);
  if (!service_.ConnectAsync(remote->host(), remote->port(), callback, error)) {
    return false;
  }

  return true;
}

}  // namespace daemon
}  // namespace dist_clang
