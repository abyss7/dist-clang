#include "daemon/commands/local_execution.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "daemon/balancer.h"
#include "net/connection.h"
#include "proto/remote.pb.h"

#include <functional>
#include <string>

using std::string;
using namespace std::placeholders;

namespace dist_clang {
namespace daemon {
namespace command {

// static
CommandPtr LocalExecution::Create(net::ConnectionPtr connection,
                                  const Local &message,
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
  if (!process.Run(10, nullptr)) {
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
  string output_path =
      message_.current_dir() + "/" + message_.cc_flags().output();
  if(SearchCache(&cache_entry)) {
    if (base::CopyFile(cache_entry.first, output_path, true)) {
      Error error;
      error.set_code(Error::OK);
      error.set_description(cache_entry.second);
      connection_->SendAsync(error);
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
      std::bind(&This::DoRemoteCompilation, this, _1, _2);
  Remote remote;
  remote.set_pp_source(pp_source_);
  remote.mutable_cc_flags()->CopyFrom(message_.cc_flags());
  if (!remote_connection->SendAsync(remote, do_remote_compilation))
    DoLocalCompilation();
}

LocalExecution::LocalExecution(net::ConnectionPtr connection,
                               const Local &message,
                               Balancer* balancer,
                               FileCache *cache)
  : connection_(connection), message_(message), balancer_(balancer),
    cache_(cache) {}

void LocalExecution::DoLocalCompilation() {
  Error error;
  const proto::Flags& flags = message_.cc_flags();

  base::Process process(flags.compiler().path(), message_.current_dir());
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg(flags.output())
         .AppendArg(flags.input());

  if (!process.Run(10, nullptr)) {
    error.set_code(Error::EXECUTION);
    error.set_description(process.stderr());
  } else {
    error.set_code(Error::OK);
    error.set_description(process.stderr());
    UpdateCache(error);
  }

  connection_->SendAsync(error);
}

bool LocalExecution::DoRemoteCompilation(net::ConnectionPtr connection,
                                         const Error& error) {
  if (error.code() != Error::OK) {
    std::cerr << error.description() << std::endl;
    return false;
  }

  auto done_remote_compilation =
      std::bind(&This::DoneRemoteCompilation, this, _1, _2, _3);
  if (!connection->ReadAsync(done_remote_compilation)) {
    DoLocalCompilation();
    return false;
  }

  return true;
}

bool LocalExecution::DoneRemoteCompilation(net::ConnectionPtr /* connection */,
                                           const Message& message,
                                           const Error& error) {
  if (error.code() != Error::OK) {
    std::cerr << error.description() << std::endl;
    DoLocalCompilation();
  }
  else if (message.HasExtension(Error::error)) {
    const Error error = message.GetExtension(Error::error);
    if (error.code() != Error::OK) {
      std::cerr << error.description() << std::endl;
      DoLocalCompilation();
    }
  }
  else if (message.HasExtension(Result::result)) {
    const Result& result = message.GetExtension(Result::result);
    string output_path =
        message_.current_dir() + "/" + message_.cc_flags().output();
    if (!base::WriteFile(output_path, result.obj()))
      DoLocalCompilation();
    else {
      Error error;
      error.set_code(Error::OK);
      connection_->SendAsync(error);
      UpdateCache(error);
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
  string command_line = base::JoinString<' '>(flags.other().begin(),
                                              flags.other().end());
  return cache_->Find(pp_source_, command_line, flags.compiler().version(),
                      entry);
}

void LocalExecution::UpdateCache(const Error& error) {
  if (!cache_ || pp_source_.empty() ||
      !message_.cc_flags().compiler().has_version())
    return;
  assert(error.code() == Error::OK);

  const proto::Flags& flags = message_.cc_flags();
  FileCache::Entry entry;
  entry.first = message_.current_dir() + "/" + flags.output();
  entry.second = error.description();
  string command_line = base::JoinString<' '>(flags.other().begin(),
                                              flags.other().end());
  cache_->Store(pp_source_, command_line, flags.compiler().version(),
                entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
