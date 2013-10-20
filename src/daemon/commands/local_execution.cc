#include "daemon/commands/local_execution.h"

#include "base/assert.h"
#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "daemon/daemon.h"
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
                                  Daemon& daemon) {
  if (!message.cc_flags().has_output() ||
      !message.cc_flags().has_input() ||
       message.cc_flags().input() == "-" ||
      !message.pp_flags().has_input() ||
       message.pp_flags().input() == "-") {
    return CommandPtr();
  }
  return CommandPtr(new LocalExecution(connection, message, daemon));
}

LocalExecution::LocalExecution(net::ConnectionPtr connection,
                               const proto::LocalExecute& message,
                               Daemon& daemon)
  : connection_(connection), message_(message),
    daemon_(daemon) {
}

void LocalExecution::Run() {
  if (!message_.has_pp_flags() ||
      !daemon_.FillFlags(message_.mutable_pp_flags())) {
    // Without preprocessing flags we can't neither check the cache, nor do
    // a remote compilation, nor put the result back in cache.
    DoLocalCompilation();
    return;
  }

  // Redirect output to stdin in |pp_flags|.
  message_.mutable_pp_flags()->set_output("-");

  // TODO: don't do preprocessing, if we don't have cache and any remotes.
  base::Process process(message_.pp_flags(), message_.current_dir());
  if (!process.Run(10)) {
    // It usually means, that there is an error in the source code.
    // We should skip a cache check and head to local compilation.
    DoLocalCompilation();
    return;
  }
  pp_source_ = process.stdout();

  FileCache::Entry cache_entry;
  if (SearchCache(&cache_entry)) {
    std::string output_path =
        message_.current_dir() + "/" + message_.cc_flags().output();
    if (base::CopyFile(cache_entry.first, output_path, true)) {
      proto::Status status;
      status.set_code(proto::Status::OK);
      status.set_description(cache_entry.second);
      connection_->SendAsync(status);
      return;
    }
  }

  // Ask balancer, if we can delegate a compilation to some remote peer.
  if (!daemon_.balancer()) {
    DoLocalCompilation();
    return;
  }

  auto callback = std::bind(&LocalExecution::DoneRemoteConnection,
                            shared_from_this(), _1, _2);
  if (!daemon_.balancer()->Decide(callback)) {
    DoLocalCompilation();
    return;
  }
}

void LocalExecution::DoneRemoteConnection(net::ConnectionPtr connection,
                                          const std::string &error) {
  if (!connection) {
    std::cerr << "Connection to remote daemon failed";
    if (!error.empty()) {
      std::cerr << ": " << error;
    }
    std::cerr << std::endl;

    DeferLocalCompilation();
    return;
  }

  auto callback = std::bind(&LocalExecution::DoRemoteCompilation,
                            shared_from_this(), _1, _2);
  proto::RemoteExecute remote;
  remote.set_pp_source(pp_source_);
  remote.mutable_cc_flags()->CopyFrom(message_.cc_flags());

  // Filter outgoing flags.
  remote.mutable_cc_flags()->mutable_compiler()->clear_path();
  remote.mutable_cc_flags()->clear_output();
  remote.mutable_cc_flags()->clear_input();
  remote.mutable_cc_flags()->clear_dependenies();

  if (!connection->SendAsync(remote, callback)) {
    DeferLocalCompilation();
    return;
  }
}

void LocalExecution::DoLocalCompilation() {
  proto::Status status;
  if (!daemon_.FillFlags(message_.mutable_cc_flags(), &status)) {
    connection_->SendAsync(status);
    return;
  }

  proto::Status message;
  base::Process process(message_.cc_flags(), message_.current_dir());
  if (!process.Run(60)) {
    message.set_code(proto::Status::EXECUTION);
    message.set_description(process.stderr());
  } else {
    message.set_code(proto::Status::OK);
    message.set_description(process.stderr());
    std::cout << "Local compilation successful: " + message_.cc_flags().input()
              << std::endl;
    UpdateCache(message);
  }

  connection_->SendAsync(message);
}

void LocalExecution::DeferLocalCompilation() {
  auto task =
      std::bind(&LocalExecution::DoLocalCompilation, shared_from_this());
  if (!daemon_.pool()->Push(task)) {
    proto::Status status;
    status.set_code(proto::Status::OVERLOAD);
    status.set_description("Queue is full");
    connection_->SendAsync(status);
  }
}

bool LocalExecution::DoRemoteCompilation(net::ConnectionPtr connection,
                                         const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    DeferLocalCompilation();
    return false;
  }

  auto callback = std::bind(&LocalExecution::DoneRemoteCompilation,
                            shared_from_this(), _1, _2, _3);
  if (!connection->ReadAsync(callback)) {
    DeferLocalCompilation();
    return false;
  }

  return true;
}

bool LocalExecution::DoneRemoteCompilation(net::ConnectionPtr /* connection */,
                                           const proto::Universal& message,
                                           const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    std::cerr << status.description() << std::endl;
    DeferLocalCompilation();
    return false;
  }
  if (message.HasExtension(proto::Status::status)) {
    const auto& status = message.GetExtension(proto::Status::status);
    if (status.code() != proto::Status::OK) {
      std::cerr << "Remote compilation failed with error(s):" << std::endl;
      std::cerr << status.description() << std::endl;
      DeferLocalCompilation();
      return false;
    }
  }
  if (message.HasExtension(proto::RemoteResult::result)) {
    const auto& result = message.GetExtension(proto::RemoteResult::result);
    std::string output_path =
        message_.current_dir() + "/" + message_.cc_flags().output();
    if (base::WriteFile(output_path, result.obj())) {
      proto::Status status;
      status.set_code(proto::Status::OK);
      std::cout << "Remote compilation successful: "
                << message_.cc_flags().input() << std::endl;
      UpdateCache(status);
      connection_->SendAsync(status);
      return false;
    }
  }

  DeferLocalCompilation();
  return false;
}

bool LocalExecution::SearchCache(FileCache::Entry* entry) {
  if (!daemon_.cache()) {
    return false;
  }

  const auto& flags = message_.cc_flags();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  return daemon_.cache()->Find(pp_source_, command_line, version, entry);
}

void LocalExecution::UpdateCache(const proto::Status& status) {
  if (!daemon_.cache() || pp_source_.empty()) {
    return;
  }
  base::Assert(status.code() == proto::Status::OK);

  const auto& flags = message_.cc_flags();
  FileCache::Entry entry;
  entry.first = message_.current_dir() + "/" + flags.output();
  entry.second = status.description();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  daemon_.cache()->Store(pp_source_, command_line, version, entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
