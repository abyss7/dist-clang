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

  pool_.reset(new ThreadPool(config.pool_capacity()));
  pool_->Run();

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
  if (!connection) {
    assert(error.code() != proto::Error::OK);
    std::cerr << error.description() << std::endl;
    return false;
  }

  if (error.code() != proto::Error::OK) {
    net::Connection::Message outgoing_message;
    outgoing_message.mutable_error()->CopyFrom(error);
    connection->Send(outgoing_message, net::Connection::Idle());
    return true;
  }

  if (!message.has_execute()) {
    net::Connection::Message outgoing_message;
    auto error_message = outgoing_message.mutable_error();
    error_message->set_code(proto::Error::BAD_MESSAGE);
    error_message->set_description("Incoming local message doesn't have "
                                   "the execution instructions!");
    connection->Send(outgoing_message, net::Connection::Idle());
    return true;
  }

  if (!pool_->Push(std::bind(&Daemon::HandleLocalExecution, this, connection,
                             message.execute()))) {
    // TODO: organize remote compilation.
  }

  return true;
}

void Daemon::HandleLocalExecution(net::ConnectionPtr connection,
                                  const proto::Execute &execute) {
  net::Connection::Message message;
  auto error = message.mutable_error();
  base::Process process(execute.executable(), execute.current_dir());
  process.AppendArg(execute.args().begin(), execute.args().end());
  if (!process.Run(30, nullptr)) {
    error->set_code(proto::Error::EXECUTION);
    error->set_description(process.stderr());
  } else {
    error->set_code(proto::Error::OK);
  }
  connection->Send(message, net::Connection::Idle());
}

}  // namespace daemon
}  // namespace dist_clang
