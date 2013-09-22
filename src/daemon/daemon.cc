#include "daemon/daemon.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
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
  balancer_.reset(new Balancer(network_service));
  if (config.has_cache_path()) {
    cache_.reset(new FileCache(config.cache_path()));
    std::cout << "Using cache on " << config.cache_path() << std::endl;
  }

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
  if (!execute.has_pp_flags()) {
    // Without preprocessing flags we can't neither check the cache, nor do
    // a remote compilation, nor put the result back in cache.
    DoLocalCompilation(connection, execute);
    return;
  }

  const proto::Flags& flags = execute.pp_flags();
  base::Process process(flags.compiler().path(), execute.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg("-")
         .AppendArg(flags.input());

  if (!process.Run(10, nullptr)) {
    // It usually means, that there is an error in the source code.
    // We should skip a cache check and head to local compilation.
    DoLocalCompilation(connection, execute);
    return;
  }

  if (!execute.cc_flags().compiler().has_version()) {
    // We can't do a remote compilation or a cache lookup, if don't know
    // the compiler version.
    DoLocalCompilation(connection, execute);
    return;
  }

  FileCache::Entry cache_entry;
  if(DoCacheLookup(process.stdout(), execute.cc_flags(), &cache_entry)) {
    string output_path =
        execute.current_dir() + "/" + execute.cc_flags().output();
    if (base::CopyFile(cache_entry.first, output_path, true)) {
      Error error;
      error.set_code(Error::OK);
      error.set_description(cache_entry.second);
      connection->SendAsync(error);
      return;
    }
  }

  if (!pool_->InternalCount()) {
    DoLocalCompilation(connection, execute, process.stdout());
    return;
  }

  auto remote_connection = balancer_->Decide();
  if (!remote_connection) {
    DoLocalCompilation(connection, execute, process.stdout());
    return;
  }

  Remote remote;
  remote.set_code(process.stdout());
  remote.mutable_cc_flags()->CopyFrom(execute.cc_flags());
  remote_connection->SendAsync(remote, std::bind(&Daemon::DoRemoteCompilation,
                                                 this, _1, _2));
}

void Daemon::DoLocalCompilation(net::ConnectionPtr connection,
                                const Local &execute, const string& pp_code) {
  Error error;
  const proto::Flags& flags = execute.cc_flags();

  base::Process process(flags.compiler().path(), execute.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg(flags.output())
         .AppendArg(flags.input());

  if (!process.Run(10, nullptr)) {
    error.set_code(Error::EXECUTION);
    error.set_description(process.stderr());
  } else {
    error.set_code(Error::OK);
    error.set_description(process.stderr());
    DoUpdateCache(pp_code, execute, error);
  }

  connection->SendAsync(error);
}

bool Daemon::DoRemoteCompilation(net::ConnectionPtr connection,
                                 const Error &error) {
  // TODO: implement this.
  return false;
}

bool Daemon::DoCacheLookup(const string &pp_code, const proto::Flags &flags,
                           FileCache::Entry* entry) {
  if (!cache_)
    return false;

  assert(flags.compiler().has_version());
  string command_line = base::JoinString<' '>(flags.other().begin(),
                                              flags.other().end());
  return cache_->Find(pp_code, command_line, flags.compiler().version(), entry);
}

void Daemon::DoUpdateCache(const string& pp_code, const Local& execute,
                           const Error& error) {
  if (!cache_ || pp_code.empty() ||
      !execute.cc_flags().compiler().has_version())
    return;
  assert(error.code() == Error::OK);

  const proto::Flags& flags = execute.cc_flags();
  FileCache::Entry entry;
  entry.first = execute.current_dir() + "/" + flags.output();
  entry.second = error.description();
  string command_line = base::JoinString<' '>(flags.other().begin(),
                                              flags.other().end());
  cache_->Store(pp_code, command_line, flags.compiler().version(), entry);
}

}  // namespace daemon
}  // namespace dist_clang
