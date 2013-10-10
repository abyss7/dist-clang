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
  : connection_(connection), message_(message), daemon_(daemon),
    timer_("RemoteExecution") {
}

void RemoteExecution::Run() {
  // Look in the local cache first.
  FileCache::Entry cache_entry;
  if (SearchCache(&cache_entry)) {
    proto::Universal message;
    auto result = message.MutableExtension(proto::RemoteResult::result);
    if (base::ReadFile(cache_entry.first, result->mutable_obj())) {
      auto status = message.MutableExtension(proto::Status::status);
      status->set_code(proto::Status::OK);
      status->set_description(cache_entry.second);
      connection_->SendAsync(message);
      return;
    }
  }

  // Check that we have a compiler of a requested version.
  proto::Status status;
  if (!daemon_.FillFlags(message_.mutable_cc_flags(), &status)) {
    connection_->SendAsync(status);
    return;
  }

  message_.mutable_cc_flags()->set_output("-");
  message_.mutable_cc_flags()->clear_input();
  message_.mutable_cc_flags()->clear_dependenies();

  // Do local compilation. Pipe the input file to the compiler and read output
  // file from the compiler's stdout.
  base::Process process(message_.cc_flags());
  if (!process.Run(60, message_.pp_source())) {
    status.set_code(proto::Status::EXECUTION);
    status.set_description(process.stderr());
    if (!process.stderr().empty()) {
      std::cerr << "Compilation failed with error:" << std::endl;
      std::cerr << process.stderr();
      std::cerr << "Arguments:";
      for (const auto& flag: message_.cc_flags().other()) {
        std::cerr << " " << flag;
      }
      std::cerr << std::endl << std::endl;
    }
  }
  else {
    status.set_code(proto::Status::OK);
    status.set_description(process.stderr());
    std::cout << "External compilation successful" << std::endl;
  }

  proto::Universal message;
  const auto& result = proto::RemoteResult::result;
  message.MutableExtension(proto::Status::status)->CopyFrom(status);
  message.MutableExtension(result)->set_obj(process.stdout());
  connection_->SendAsync(message);
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
