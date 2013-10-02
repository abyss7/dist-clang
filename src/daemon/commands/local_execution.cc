#include "daemon/commands/local_execution.h"

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
  : connection_(connection), message_(message), daemon_(daemon) {
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
      if (!connection_->SendAsync(status)) {
        connection_->Close();
      }
      return;
    }
  }

  // Ask balancer, if we can delegate a compilation to some remote peer.
  if (!daemon_.balancer()) {
    DoLocalCompilation();
    return;
  }

  auto remote_connection = daemon_.balancer()->Decide();
  if (!remote_connection) {
    DoLocalCompilation();
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

  if (!remote_connection->SendAsync(remote, callback)) {
    DoLocalCompilation();
    return;
  }
}

void LocalExecution::DoLocalCompilation() {
  proto::Status status;
  if (!daemon_.FillFlags(message_.mutable_cc_flags(), &status)) {
    if (!connection_->SendAsync(status, net::Connection::CloseAfterSend())) {
      connection_->Close();
    }
    return;
  }

  proto::Status message;
  base::Process process(message_.cc_flags(), message_.current_dir());
  if (!process.Run(10)) {
    message.set_code(proto::Status::EXECUTION);
    message.set_description(process.stderr());
  } else {
    message.set_code(proto::Status::OK);
    message.set_description(process.stderr());
    UpdateCache(message);
  }

  if (!connection_->SendAsync(message, net::Connection::Idle(), &status)) {
    std::cerr << "Failed to send message: " << status.description()
              << std::endl;
    connection_->Close();
  }
}

void LocalExecution::DeferLocalCompilation() {
  auto task =
      std::bind(&LocalExecution::DoLocalCompilation, shared_from_this());
  if (!daemon_.pool()->Push(task)) {
    connection_->Close();
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
      UpdateCache(status);
      if (connection_->SendAsync(status)) {
        connection_->Close();
        return false;
      }
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
  return daemon_.cache()->Find(pp_source_, command_line, version, entry);
}

void LocalExecution::UpdateCache(const proto::Status& status) {
  if (!daemon_.cache() || pp_source_.empty()) {
    return;
  }
  assert(status.code() == proto::Status::OK);

  const auto& flags = message_.cc_flags();
  FileCache::Entry entry;
  entry.first = message_.current_dir() + "/" + flags.output();
  entry.second = status.description();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  daemon_.cache()->Store(pp_source_, command_line, version, entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
