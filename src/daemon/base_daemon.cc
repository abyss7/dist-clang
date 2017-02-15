#include <daemon/base_daemon.h>

#include <base/logging.h>
#include <net/connection.h>
#include <net/network_service_impl.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

bool BaseDaemon::Initialize() {
  return Check() && Reload() && network_service_->Run();
}

BaseDaemon::BaseDaemon(const Configuration& conf)
    : resolver_(net::EndPointResolver::Create()),
      conf_(std::make_shared<Configuration>(conf)),
      network_service_(net::NetworkService::Create(
          conf.connect_timeout(), conf.read_timeout(), conf.send_timeout(),
          conf.read_minimum())) {
  conf_->CheckInitialized();
}

BaseDaemon::~BaseDaemon() {
  network_service_.reset();
}

bool BaseDaemon::Update(const Configuration& conf) {
  if (!Check(conf)) {
    return false;
  }

  if (Reload(conf)) {
    UniqueLock lock(conf_mutex_);
    conf_ = std::make_shared<Configuration>(conf);
    return true;
  }

  return false;
}

void BaseDaemon::HandleNewConnection(net::ConnectionPtr connection) {
  using namespace std::placeholders;

  auto callback = std::bind(&BaseDaemon::HandleNewMessage, this, _1, _2, _3);
  connection->ReadAsync(callback);
}

}  // namespace daemon
}  // namespace dist_clang
