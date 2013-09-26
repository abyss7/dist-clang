#include "daemon/daemon.h"

#include "daemon/commands/local_execution.h"
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
  proto::Error error;

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
    std::cout << "Using cache on " << config.cache_path() << std::endl;
  }

  if (!network_service.Listen(config.socket_path(),
           std::bind(&Daemon::HandleNewConnection, this, _1), &error)) {
    std::cerr << error.description() << std::endl;
    return false;
  }

  if (config.has_local()) {
    network_service.Listen(config.local().host(), config.local().port(),
                           std::bind(&Daemon::HandleNewConnection, this, _1));
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
                              const proto::Error& error) {
  if (error.code() != proto::Error::OK) {
    std::cerr << error.description() << std::endl;
    return connection->SendAsync(error, net::Connection::CloseAfterSend());
  }

  command::CommandPtr command;
  if (message.HasExtension(proto::LocalExecute::local)) {
    const proto::LocalExecute& local =
        message.GetExtension(proto::LocalExecute::local);
    command = command::LocalExecution::Create(connection, local,
                                              balancer_.get(), cache_.get());
  }

  return pool_->Push(std::bind(&command::Command::Run, command));
}

}  // namespace daemon
}  // namespace dist_clang
