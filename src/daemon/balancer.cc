#include "daemon/balancer.h"

#include "base/random.h"
#include "net/base/end_point.h"
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
  auto end_point = net::EndPoint::TcpHost(remote.host(), remote.port());
  if (end_point) {
    remotes_.insert(std::make_pair(remote, end_point));
  }
}

bool Balancer::Decide(const ConnectCallback& callback, std::string* error) {
  do {
    auto remote_index = index_.fetch_add(1) % (remotes_.size() + 1);
    if (remote_index == remotes_.size()) {
      return false;
    }

    auto remote = remotes_.cbegin();
    std::advance(remote, remote_index);
    if (remote->second.use_count() > remote->first.threads()) {
      continue;
    }
    if (!service_.ConnectAsync(remote->second, callback, error)) {
      return false;
    }
    return true;
  }
  while(true);

  return true;
}

}  // namespace daemon
}  // namespace dist_clang
