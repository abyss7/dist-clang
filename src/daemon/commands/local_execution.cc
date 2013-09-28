#include "daemon/commands/local_execution.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "daemon/balancer.h"
#include "net/connection.h"

#include <functional>
#include <iostream>

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {
namespace command {

// static
CommandPtr LocalExecution::Create(net::ConnectionPtr connection,
                                  const proto::LocalExecute& message,
                                  Balancer* balancer,
                                  FileCache* cache) {
  return CommandPtr(new LocalExecution(connection, message, balancer, cache));
}

void LocalExecution::Run() {
  if (!message_.has_pp_flags()) {
    // Without preprocessing flags we can't neither check the cache, nor do
    // a remote compilation, nor put the result back in cache.
    DoLocalCompilation();
    return;
  }

  const proto::Flags& flags = message_.pp_flags();
  base::Process process(flags.compiler().path(), message_.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg("-")
         .AppendArg(flags.input());
  if (!process.Run(10)) {
    // It usually means, that there is an error in the source code.
    // We should skip a cache check and head to local compilation.
    DoLocalCompilation();
    return;
  }
  pp_source_ = process.stdout();

  if (!message_.cc_flags().compiler().has_version()) {
    // We can't do a remote compilation or a cache lookup, if don't know
    // the compiler version.
    DoLocalCompilation();
    return;
  }

  FileCache::Entry cache_entry;
  std::string output_path =
      message_.current_dir() + "/" + message_.cc_flags().output();
  if(SearchCache(&cache_entry)) {
    if (base::CopyFile(cache_entry.first, output_path, true)) {
      proto::Status status;
      status.set_code(proto::Status::OK);
      status.set_description(cache_entry.second);
      connection_->SendAsync(status);
      return;
    }
  }

  // Ask balancer, if we can delegate a compilation to some remote peer.
  auto remote_connection = balancer_->Decide();
  if (!remote_connection) {
    DoLocalCompilation();
    return;
  }

  auto do_remote_compilation =
      std::bind(&LocalExecution::DoRemoteCompilation, this, _1, _2);
  proto::RemoteExecute remote;
  remote.set_pp_source(pp_source_);
  remote.mutable_cc_flags()->CopyFrom(message_.cc_flags());
  if (!remote_connection->SendAsync(remote, do_remote_compilation))
    DoLocalCompilation();
}

LocalExecution::LocalExecution(net::ConnectionPtr connection,
                               const proto::LocalExecute& message,
                               Balancer* balancer,
                               FileCache *cache)
  : connection_(connection), message_(message), balancer_(balancer),
    cache_(cache) {}

void LocalExecution::DoLocalCompilation() {
  proto::Status status;
  const proto::Flags& flags = message_.cc_flags();

  base::Process process(flags.compiler().path(), message_.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg(flags.output())
         .AppendArg(flags.input());

  if (!process.Run(10)) {
    status.set_code(proto::Status::EXECUTION);
    status.set_description(process.stderr());
  } else {
    status.set_code(proto::Status::OK);
    status.set_description(process.stderr());
    UpdateCache(status);
  }

  connection_->SendAsync(status);
}

bool LocalExecution::DoRemoteCompilation(net::ConnectionPtr connection,
                                         const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    return false;
  }

  auto done_remote_compilation =
      std::bind(&LocalExecution::DoneRemoteCompilation, this, _1, _2, _3);
  if (!connection->ReadAsync(done_remote_compilation)) {
    DoLocalCompilation();
    return false;
  }

  return true;
}

bool LocalExecution::DoneRemoteCompilation(net::ConnectionPtr /* connection */,
                                           const proto::Universal& message,
                                           const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    DoLocalCompilation();
  }
  else if (message.HasExtension(proto::Status::status)) {
    const proto::Status& status = message.GetExtension(proto::Status::status);
    if (status.code() != proto::Status::OK) {
      std::cerr << "Remote compilation failed with error(s):" << std::endl;
      std::cerr << status.description() << std::endl;
      DoLocalCompilation();
    }
  }
  else if (message.HasExtension(proto::RemoteResult::result)) {
    const proto::RemoteResult& result =
        message.GetExtension(proto::RemoteResult::result);
    std::string output_path =
        message_.current_dir() + "/" + message_.cc_flags().output();
    if (!base::WriteFile(output_path, result.obj()))
      DoLocalCompilation();
    else {
      proto::Status status;
      status.set_code(proto::Status::OK);
      connection_->SendAsync(status);
      UpdateCache(status);
    }
  }
  else
    DoLocalCompilation();

  return false;
}

bool LocalExecution::SearchCache(FileCache::Entry* entry) {
  if (!cache_)
    return false;

  const proto::Flags& flags = message_.cc_flags();
  assert(flags.compiler().has_version());
  const std::string& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  return cache_->Find(pp_source_, command_line, version, entry);
}

void LocalExecution::UpdateCache(const proto::Status& status) {
  if (!cache_ || pp_source_.empty() ||
      !message_.cc_flags().compiler().has_version())
    return;
  assert(status.code() == proto::Status::OK);

  const proto::Flags& flags = message_.cc_flags();
  FileCache::Entry entry;
  entry.first = message_.current_dir() + "/" + flags.output();
  entry.second = status.description();
  const std::string& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  cache_->Store(pp_source_, command_line, version, entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
