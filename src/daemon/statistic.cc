#include "daemon/statistic.h"

#include "net/connection.h"
#include "net/network_service.h"
#include "proto/config.pb.h"

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

bool Statistic::Initialize(net::NetworkService& network_service,
                           const proto::Host& host) {
  auto callback = std::bind(&Statistic::HandleNewConnection, this, _1);
  return network_service.Listen(host.host(), host.port(), callback);
}

void Statistic::HandleNewConnection(net::ConnectionPtr connection) {

}

}  // namespace daemon
}  // namespace dist_clang
