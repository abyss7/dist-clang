#pragma once

#include "base/testable.h"
#include "net/connection_forward.h"

namespace dist_clang {

namespace net {
class NetworkService;
}  // namespace net

namespace proto {
class Host;
}  // namespace proto

namespace daemon {

class Statistic : public base::Testable<Statistic> {
 public:
  bool Initialize(net::NetworkService& network_service,
                  const proto::Host& host);

 private:
  void HandleNewConnection(net::ConnectionPtr connection);
};

}  // namespace daemon
}  // namespace dist_clang
