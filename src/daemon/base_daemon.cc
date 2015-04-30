#include <daemon/base_daemon.h>

#include <base/logging.h>
#include <net/connection.h>
#include <net/network_service_impl.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

bool BaseDaemon::Initialize() {
  return network_service_->Run();
}

BaseDaemon::BaseDaemon(const proto::Configuration& configuration)
    : resolver_(net::EndPointResolver::Create()),
      network_service_(net::NetworkService::Create(
          configuration.read_timeout(), configuration.send_timeout(),
          configuration.read_minimum())) {
}

BaseDaemon::~BaseDaemon() {
  network_service_.reset();
}

void BaseDaemon::HandleNewConnection(net::ConnectionPtr connection) {
  using namespace std::placeholders;

  auto callback = std::bind(&BaseDaemon::HandleNewMessage, this, _1, _2, _3);
  connection->ReadAsync(callback);
}

}  // namespace daemon
}  // namespace dist_clang
