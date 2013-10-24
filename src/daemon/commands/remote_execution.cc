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
                                   ScopedMessage message,
                                   Daemon& daemon) {
  return CommandPtr(new RemoteExecution(connection, std::move(message),
                                        daemon));
}

RemoteExecution::RemoteExecution(net::ConnectionPtr connection,
                                 ScopedMessage message,
                                 Daemon& daemon)
  : connection_(connection), message_(std::move(message)), daemon_(daemon) {
}

void RemoteExecution::Run() {
  // Look in the local cache first.
  FileCache::Entry cache_entry;
  if (SearchCache(&cache_entry)) {
    Daemon::ScopedMessage message(new proto::Universal);
    auto result = message->MutableExtension(proto::RemoteResult::result);
    if (base::ReadFile(cache_entry.first, result->mutable_obj())) {
      auto status = message->MutableExtension(proto::Status::status);
      status->set_code(proto::Status::OK);
      status->set_description(cache_entry.second);
      connection_->SendAsync(std::move(message));
      return;
    }
  }

  // Check that we have a compiler of a requested version.
  proto::Status status;
  if (!daemon_.FillFlags(message_->mutable_cc_flags(), &status)) {
    connection_->ReportStatus(status);
    return;
  }

  message_->mutable_cc_flags()->set_output("-");
  message_->mutable_cc_flags()->clear_input();
  message_->mutable_cc_flags()->clear_dependenies();

  // Optimize compilation for preprocessed code for some languages.
  if (message_->cc_flags().has_language()) {
    if (message_->cc_flags().language() == "c") {
      message_->mutable_cc_flags()->set_language("cpp-output");
    }
    else if (message_->cc_flags().language() == "c++") {
      message_->mutable_cc_flags()->set_language("c++-cpp-output");
    }
  }

  // Do local compilation. Pipe the input file to the compiler and read output
  // file from the compiler's stdout.
  std::string error;
  base::Process process(message_->cc_flags());
  if (!process.Run(60, message_->pp_source(), &error)) {
    status.set_code(proto::Status::EXECUTION);
    status.set_description(process.stderr());
    if (!process.stdout().empty() || !process.stderr().empty()) {
      std::cerr << "Compilation failed with error:" << std::endl;
      std::cerr << process.stderr() << std::endl;
      std::cerr << process.stdout() << std::endl;
    }
    else if (!error.empty()) {
      std::cerr << "Compilation failed with error: " << error << std::endl;
    }
    else {
      std::cerr << "Compilation failed without errors" << std::endl;
    }
    std::cerr << "Arguments:";
    for (const auto& flag: message_->cc_flags().other()) {
      std::cerr << " " << flag;
    }
    std::cerr << std::endl << std::endl;
  }
  else {
    status.set_code(proto::Status::OK);
    status.set_description(process.stderr());
    std::cout << "External compilation successful" << std::endl;
  }

  Daemon::ScopedMessage message(new proto::Universal);
  const auto& result = proto::RemoteResult::result;
  message->MutableExtension(proto::Status::status)->CopyFrom(status);
  message->MutableExtension(result)->set_obj(process.stdout());
  connection_->SendAsync(std::move(message));
}

bool RemoteExecution::SearchCache(FileCache::Entry *entry) {
  if (!daemon_.cache())
    return false;

  const auto& flags = message_->cc_flags();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  return daemon_.cache()->Find(message_->pp_source(), command_line, version,
                               entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
