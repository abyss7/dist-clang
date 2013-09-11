#include "daemon/daemon.h"

#include "daemon/configuration.h"
#include "net/connection.h"
#include "net/network_service.h"

#include <iostream>

using std::string;
using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

bool Daemon::Initialize(const Configuration &configuration,
                        net::NetworkService &network_service) {
  const proto::Configuration& config = configuration.config();
  string error;

  if (!config.IsInitialized()) {
    std::cerr << config.InitializationErrorString() << std::endl;
    return false;
  }

  if (!network_service.Listen(config.socket_path(),
           std::bind(&Daemon::HandleNewConnection, this, false, _1), &error)) {
    std::cerr << error << std::endl;
    return false;
  }

  return true;
}

void Daemon::AddConnectionToBalancer(net::ConnectionPtr connection) {
  // TODO: implement this.
}

void Daemon::HandleNewConnection(bool remote, net::ConnectionPtr connection) {
  if (remote)
    AddConnectionToBalancer(connection);
  connection->Read(std::bind(&Daemon::HandleIncomingMessage, this, _1, _2));
}

void Daemon::HandleIncomingMessage(net::ConnectionPtr connection,
                                   const net::Connection::Message& message) {
  // TODO: implement this.
}

}  // namespace daemon
}  // namespace dist_clang
