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
  Error error;

  if (!config.IsInitialized()) {
    std::cerr << config.InitializationErrorString() << std::endl;
    return false;
  }

  pool_.reset(new ThreadPool(config.pool_capacity()));
  pool_->Run();

  if (!network_service.Listen(config.socket_path(),
           std::bind(&Daemon::HandleNewConnection, this, _1), &error)) {
    std::cerr << error.description() << std::endl;
    return false;
  }

  return true;
}

void Daemon::HandleNewConnection(net::ConnectionPtr connection) {
  connection->ReadAsync(std::bind(&Daemon::HandleNewMessage, this, _1, _2, _3));
}

bool Daemon::HandleNewMessage(net::ConnectionPtr connection,
                              const net::Connection::Message& message,
                              const Error& error) {
  if (!connection) {
    assert(error.code() != Error::OK);
    std::cerr << error.description() << std::endl;
    return false;
  }

  if (error.code() != Error::OK) {
    connection->SendAsync(error);
    return true;
  }

  if (message.HasExtension(Local::local)) {
    pool_->PushInternal(std::bind(&Daemon::DoLocalExecution, this,
                                  connection,
                                  message.GetExtension(Local::local)));
  }

  return true;
}

void Daemon::DoLocalExecution(net::ConnectionPtr connection,
                              const Local &execute) {
  const proto::Flags& flags = execute.pp_flags();
  base::Process process(flags.executable(), execute.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg("-")
         .AppendArg(flags.input());

  if (!process.Run(10, nullptr)) {
    // It usually means, that there is an error in the source code.
    // We should skip a cache check and head to local compilation.
    DoLocalCompilation(connection, execute, true);
    return;
  }

  // TODO: check the cache.

  // TODO: at this point the balancer should decide, if we compile locally,
  // or remotely.

  DoLocalCompilation(connection, execute, true);
}

void Daemon::DoLocalCompilation(net::ConnectionPtr connection,
                                const Local &execute, bool update_cache) {
  Error error;
  const proto::Flags& flags = execute.cc_flags();
  base::Process process(flags.executable(), execute.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg(flags.output())
         .AppendArg(flags.input());

  if (!process.Run(10, nullptr)) {
    error.set_code(Error::EXECUTION);
    error.set_description(process.stderr());
  } else {
    error.set_code(Error::OK);
    if (update_cache) {
      // TODO: update cache async.
    }
  }

  connection->SendAsync(error);
}

}  // namespace daemon
}  // namespace dist_clang
