#include "daemon/daemon.h"

#include "daemon/commands/local_execution.h"
#include "daemon/commands/remote_execution.h"
#include "daemon/configuration.h"
#include "net/connection.h"
#include "net/network_service.h"
#include "proto/config.pb.h"

#include <functional>
#include <iostream>

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

bool Daemon::Initialize(const Configuration &configuration,
                        net::NetworkService &network_service) {
  const proto::Configuration& config = configuration.config();

  if (!config.IsInitialized()) {
    std::cerr << config.InitializationErrorString() << std::endl;
    return false;
  }

  if (config.has_local())
    pool_.reset(new ThreadPool(config.pool_capacity(),
                               config.local().threads()));
  else
    pool_.reset(new ThreadPool(config.pool_capacity()));
  pool_->Run();

  balancer_.reset(new Balancer(network_service));

  if (config.has_cache_path()) {
    cache_.reset(new FileCache(config.cache_path()));
  }

  auto handle_new_conn = std::bind(&Daemon::HandleNewConnection, this, _1);
  if (!network_service.Listen(config.socket_path(), handle_new_conn)) {
    return false;
  }

  if (config.has_local()) {
    network_service.Listen(config.local().host(), config.local().port(),
                           handle_new_conn);
  }

  // TODO: handle config.remotes

  for (auto version: config.versions()) {
    compilers_.insert(std::make_pair(version.version(), version.path()));
  }

  return true;
}

void Daemon::HandleNewConnection(net::ConnectionPtr connection) {
  connection->ReadAsync(std::bind(&Daemon::HandleNewMessage, this, _1, _2, _3));
}

bool Daemon::HandleNewMessage(net::ConnectionPtr connection,
                              const proto::Universal& message,
                              const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    return connection->SendAsync(status, net::Connection::CloseAfterSend());
  }

  command::CommandPtr command;
  if (message.HasExtension(proto::LocalExecute::local)) {
    const proto::LocalExecute& local =
        message.GetExtension(proto::LocalExecute::local);
    command = command::LocalExecution::Create(connection, local,
                                              balancer_.get(), cache_.get());
  }
  else if (message.HasExtension(proto::RemoteExecute::remote)) {
    const proto::RemoteExecute& remote =
        message.GetExtension(proto::RemoteExecute::remote);
    command = command::RemoteExecution::Create(connection, remote,
                                               cache_.get(), &compilers_);
  }

  return pool_->Push(std::bind(&command::Command::Run, command));
}

}  // namespace daemon
}  // namespace dist_clang
