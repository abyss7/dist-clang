#include "daemon/commands/remote_execution.h"

#include "base/file_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "net/connection.h"

namespace dist_clang {
namespace daemon {
namespace command {

using CompilerMap = std::unordered_map<std::string, std::string>;
using Remote = proto::RemoteResult;

// static
CommandPtr RemoteExecution::Create(net::ConnectionPtr connection,
                                   const proto::RemoteExecute& remote,
                                   FileCache* cache,
                                   const CompilerMap* compilers) {
  return CommandPtr(new RemoteExecution(connection, remote, cache, compilers));
}

void RemoteExecution::Run() {
  proto::Error error;

  // Look in the local cache first.
  FileCache::Entry cache_entry;
  if(SearchCache(&cache_entry)) {
    proto::Universal message;
    auto result = message.MutableExtension(proto::RemoteResult::result);
    if (base::ReadFile(cache_entry.first, result->mutable_obj())) {
      auto error = message.MutableExtension(proto::Error::error);
      error->set_code(proto::Error::OK);
      error->set_description(cache_entry.second);
      if (!connection_->SendAsync(message))
        connection_->Close();
      return;
    }
  }

  // Check that remote peer has provided us with a compiler version - otherwise,
  // we won't be able to do local compilation.
  if (!message_.cc_flags().compiler().has_version()) {
    error.set_code(proto::Error::BAD_MESSAGE);
    error.set_description("The compiler version is not specified");
    connection_->SendAsync(error, net::Connection::CloseAfterSend());
    return;
  }

  // Check that we have a compiler of a requested version.
  const proto::Flags& flags = message_.cc_flags();
  auto compiler = compilers_->find(flags.compiler().version());
  if (compiler == compilers_->end()) {
    error.set_code(proto::Error::NO_VERSION);
    error.set_description("Compiler of a requested version is missing");
    connection_->SendAsync(error, net::Connection::CloseAfterSend());
    return;
  }

  // Do local compilation. Pipe the input file to the compiler and read output
  // file from the compiler's stdout.
  base::Process process(compiler->second);
  process.AppendArg(flags.other().begin(), flags.other().end())
         .AppendArg("-o").AppendArg("-");
  if (!process.Run(30, message_.pp_source())) {
    error.set_code(proto::Error::EXECUTION);
    error.set_description(process.stderr());
  } else {
    error.set_code(proto::Error::OK);
    error.set_description(process.stderr());
  }

  proto::Universal message;
  message.MutableExtension(proto::Error::error)->CopyFrom(error);
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
