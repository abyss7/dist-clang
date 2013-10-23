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
  const auto& config = configuration.config();

  if (!config.IsInitialized()) {
    std::cerr << config.InitializationErrorString() << std::endl;
    return false;
  }

  if (config.has_local()) {
    pool_.reset(new base::ThreadPool(config.pool_capacity(),
                                     config.local().threads()));
  }
  else {
    pool_.reset(new base::ThreadPool(config.pool_capacity()));
  }
  pool_->Run();

  if (config.has_cache_path()) {
    cache_.reset(new FileCache(config.cache_path()));
  }

  auto callback = std::bind(&Daemon::HandleNewConnection, this, _1);
  bool is_listening = false;
  if (config.has_local()) {
    std::string error;
    bool result = network_service.Listen(config.local().host(),
                                         config.local().port(),
                                         callback, &error);
    if (!result) {
      std::cerr << "Failed to listen on " << config.local().host() << ":"
                << config.local().port() << " : " << error << std::endl;
    }
    is_listening |= result;
  }

  if (config.has_socket_path()) {
    is_listening |= network_service.Listen(config.socket_path(), callback);
  }

  if (!is_listening) {
    std::cerr << "Daemon is not listening. Quitting..." << std::endl;
    return false;
  }

  balancer_.reset(new Balancer(network_service));
  for (const auto& remote: config.remotes()) {
    balancer_->AddRemote(remote);
  }

  for (const auto& version: config.versions()) {
    compilers_.insert(std::make_pair(version.version(), version.path()));

    // Load plugins.
    auto value = std::make_pair(version.version(), PluginNameMap());
    auto& plugin_map = plugins_.insert(value).first->second;
    for (const auto& plugin: version.plugins()) {
      plugin_map.insert(std::make_pair(plugin.name(), plugin.path()));
    }
  }

  return true;
}

bool Daemon::FillFlags(proto::Flags* flags, proto::Status* status) {
  // No flags - filled flags.
  if (!flags) {
    return true;
  }

  if (!flags->compiler().has_path()) {
    auto compiler = compilers_.find(flags->compiler().version());
    if (compiler == compilers_.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Compiler of the required version not found");
      }
      return false;
    }
    flags->mutable_compiler()->set_path(compiler->second);
  }

  auto plugin_map = plugins_.find(flags->compiler().version());
  auto& plugins = *flags->mutable_compiler()->mutable_plugins();
  for (auto& plugin: plugins) {
    if (!plugin.has_path() && plugin_map == plugins_.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Plugin " + plugin.name() + " not found");
      }
      return false;
    }
    auto plugin_by_name = plugin_map->second.find(plugin.name());
    if (plugin_by_name == plugin_map->second.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Plugin " + plugin.name() + " not found");
      }
      return false;
    }
    plugin.set_path(plugin_by_name->second);
  }

  return true;
}

void Daemon::HandleNewConnection(net::ConnectionPtr connection) {
  auto callback = std::bind(&Daemon::HandleNewMessage, this, _1, _2, _3);
  connection->ReadAsync(callback);
}

bool Daemon::HandleNewMessage(net::ConnectionPtr connection,
                              const proto::Universal& message,
                              const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    return connection->SendAsync(status);
  }

  command::CommandPtr command;
  if (message.HasExtension(proto::LocalExecute::local)) {
    const auto& local = message.GetExtension(proto::LocalExecute::local);
    command = command::LocalExecution::Create(connection, local, *this);
  }
  else if (message.HasExtension(proto::RemoteExecute::remote)) {
    const auto& remote = message.GetExtension(proto::RemoteExecute::remote);
    command = command::RemoteExecution::Create(connection, remote, *this);
  }

  if (!command) {
    return false;
  }
  return pool_->Push(std::bind(&command::Command::Run, command));
}

}  // namespace daemon
}  // namespace dist_clang
