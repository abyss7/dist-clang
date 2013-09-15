#include "daemon/daemon.h"

#include "base/process.h"
#include "daemon/configuration.h"
#include "net/connection.h"
#include "net/network_service.h"
#include "proto/utils.h"

#include <iostream>

using std::string;
using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

bool Daemon::Initialize(const Configuration &configuration,
                        net::NetworkService &network_service) {
  const proto::Configuration& config = configuration.config();
  proto::Error error;

  if (!config.IsInitialized()) {
    std::cerr << config.InitializationErrorString() << std::endl;
    return false;
  }

  if (!network_service.Listen(config.socket_path(),
           std::bind(&Daemon::HandleNewConnection, this, false, _1), &error)) {
    std::cerr << error.description() << std::endl;
    return false;
  }

  return true;
}

void Daemon::HandleNewConnection(bool remote, net::ConnectionPtr connection) {
  connection->Read(std::bind(&Daemon::HandleLocalMessage, this, _1, _2, _3));
}

bool Daemon::HandleLocalMessage(net::ConnectionPtr connection,
                                const net::Connection::Message& message,
                                const proto::Error& error) {
  proto::PrintMessage(message);

  if (!connection && error.code() != proto::Error::OK) {
    // TODO: send back error.
    return false;
  }

  if (!message.has_execute()) {
    // TODO: send back error.
    return false;
  }

  const proto::Execute& execute = message.execute();
  base::Process process(execute.executable(), execute.current_dir());
  process.AppendArg(execute.args().begin(), execute.args().end());
  if (!process.Run(30, nullptr))
    return false;

  return true;
}

}  // namespace daemon
}  // namespace dist_clang
