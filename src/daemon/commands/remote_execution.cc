#include "daemon/commands/remote_execution.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "daemon/daemon.h"
#include "net/connection.h"

namespace dist_clang {
namespace daemon {
namespace command {

// static
CommandPtr RemoteExecution::Create(net::ConnectionPtr connection,
                                   const proto::RemoteExecute& message,
                                   Daemon& daemon) {
  return CommandPtr(new RemoteExecution(connection, message, daemon));
}

RemoteExecution::RemoteExecution(net::ConnectionPtr connection,
                                 const proto::RemoteExecute& message,
                                 Daemon& daemon)
  : connection_(connection), message_(message), daemon_(daemon) {
}

void RemoteExecution::Run() {
  using Remote = proto::RemoteResult;

  // Look in the local cache first.
  FileCache::Entry cache_entry;
  if (SearchCache(&cache_entry)) {
    proto::Universal message;
    auto result = message.MutableExtension(proto::RemoteResult::result);
    if (base::ReadFile(cache_entry.first, result->mutable_obj())) {
      auto status = message.MutableExtension(proto::Status::status);
      status->set_code(proto::Status::OK);
      status->set_description(cache_entry.second);
      if (!connection_->SendAsync(message))
        connection_->Close();
      return;
    }
  }

  // Check that we have a compiler of a requested version.
  proto::Status status;
  if (!daemon_.FillFlags(message_.mutable_cc_flags(), &status)) {
    if (!connection_->SendAsync(status, net::Connection::CloseAfterSend())) {
      connection_->Close();
    }
    return;
  }

  // Filter out flags (-MMD, -MF, ...) for '*.d' file generation, since it's
  // generated on a preprocessing phase and will fail local compilation.
  const auto& flags = message_.cc_flags();
  for (int i = 0; i < flags.other_size(); ++i) {
    if (flags.other(i) == "-MMD") {
      message_.mutable_cc_flags()->mutable_other(i)->clear();
    }
    else if (flags.other(i) == "-MF") {
      message_.mutable_cc_flags()->mutable_other(i)->clear();
      message_.mutable_cc_flags()->mutable_other(++i)->clear();
    }
    else if (flags.other(i) == "-dependency-file") {
      message_.mutable_cc_flags()->mutable_other(i)->clear();
      message_.mutable_cc_flags()->mutable_other(++i)->clear();
    }
  }

  message_.mutable_cc_flags()->set_output("-");
  message_.mutable_cc_flags()->clear_input();

  // Do local compilation. Pipe the input file to the compiler and read output
  // file from the compiler's stdout.
  base::Process process(flags);
  if (!process.Run(30, message_.pp_source())) {
    status.set_code(proto::Status::EXECUTION);
    status.set_description(process.stderr());
    std::cerr << "Compilation failed with error:" << std::endl;
    std::cerr << process.stderr();
    std::cerr << "Arguments:";
    for (auto flag: flags.other()) {
      std::cerr << " " << flag;
    }
    std::cerr << std::endl << std::endl;
  } else {
    status.set_code(proto::Status::OK);
    status.set_description(process.stderr());
  }

  proto::Universal message;
  message.MutableExtension(proto::Status::status)->CopyFrom(status);
  message.MutableExtension(Remote::result)->set_obj(process.stdout());
  if (!connection_->SendAsync(message)) {
    connection_->Close();
  }
}

bool RemoteExecution::SearchCache(FileCache::Entry *entry) {
  if (!daemon_.cache())
    return false;

  const auto& flags = message_.cc_flags();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  return
      daemon_.cache()->Find(message_.pp_source(), command_line, version, entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
