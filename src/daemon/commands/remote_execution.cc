#include "daemon/commands/remote_execution.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "net/connection.h"

namespace dist_clang {
namespace daemon {
namespace command {

// static
CommandPtr RemoteExecution::Create(net::ConnectionPtr connection,
                                   const proto::RemoteExecute& remote,
                                   FileCache* cache,
                                   const CompilerMap* compilers) {
  return CommandPtr(new RemoteExecution(connection, remote, cache, compilers));
}

void RemoteExecution::Run() {
  using Remote = proto::RemoteResult;

  proto::Status status;

  // Look in the local cache first.
  FileCache::Entry cache_entry;
  if(SearchCache(&cache_entry)) {
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

  // Check that remote peer has provided us with a compiler version - otherwise,
  // we won't be able to do local compilation.
  if (!message_.cc_flags().compiler().has_version()) {
    status.set_code(proto::Status::BAD_MESSAGE);
    status.set_description("The compiler version is not specified");
    connection_->SendAsync(status, net::Connection::CloseAfterSend());
    return;
  }

  // Check that we have a compiler of a requested version.
  const proto::Flags& flags = message_.cc_flags();
  auto compiler = compilers_->find(flags.compiler().version());
  if (compiler == compilers_->end()) {
    status.set_code(proto::Status::NO_VERSION);
    status.set_description("Compiler of a requested version is missing");
    connection_->SendAsync(status, net::Connection::CloseAfterSend());
    return;
  }

  // Filter out flags (-MMD, -MF, ...) for '*.d' file generation, since it's
  // generated on a preprocessing phase and will fail local compilation.
  for (size_t i = 0; i < flags.other_size(); ++i) {
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

  // Do local compilation. Pipe the input file to the compiler and read output
  // file from the compiler's stdout.
  base::Process process(compiler->second);
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg("-");
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
  connection_->SendAsync(message);
}

RemoteExecution::RemoteExecution(net::ConnectionPtr connection,
                                 const proto::RemoteExecute& remote,
                                 FileCache* cache,
                                 const CompilerMap* compilers)
  : connection_(connection), message_(remote), cache_(cache),
    compilers_(compilers) {}

bool RemoteExecution::SearchCache(FileCache::Entry *entry) {
  if (!cache_)
    return false;

  const proto::Flags& flags = message_.cc_flags();
  assert(flags.compiler().has_version());
  const std::string& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  return cache_->Find(message_.pp_source(), command_line, version, entry);
}

}  // namespace command
}  // namespace daemon
}  // namespace dist_clang
