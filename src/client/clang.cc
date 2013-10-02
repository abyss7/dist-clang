#include "base/c_utils.h"
#include "base/constants.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "client/clang_flag_set.h"
#include "net/connection.h"
#include "net/network_service.h"
#include "proto/remote.pb.h"
#include "proto/utils.h"

#include <iostream>
#include <list>
#include <string>

using namespace dist_clang;

namespace {

const char* kEnvClangdSocket = "CLANGD_SOCKET_PATH";
const char* kEnvClangdCxx = "CLANGD_CXX";

int ExecuteLocally(char* argv[]) {
  std::string clangd_cxx_path = base::GetEnv(kEnvClangdCxx);
  if (clangd_cxx_path.empty()) {
    std::cerr << "Provide real clang++ compiler path via "
              << kEnvClangdCxx << std::endl;
    return 1;
  }

  if (execv(clangd_cxx_path.c_str(), argv) == -1) {
    std::cerr << "Local execution failed: "
              << strerror(errno) << std::endl;
    return 1;
  }

  return 0;
}

// The clang output has following format:
//
// clang version 3.4 (...)
// Target: x86_64-unknown-linux-gnu
// Thread model: posix
//  "/path/to/clang" "-cc1" "-triple" ...
//
// Pay attention to the leading space in the fourth line.
bool ParseClangOutput(const std::string& output,
                      std::string* version,
                      client::ClangFlagSet::StringList& args) {
  client::ClangFlagSet::StringList lines;
  base::SplitString<'\n'>(output, lines);
  if (lines.size() != 4)
    // FIXME: we don't support composite tasks yet.
    return false;

  if (version)
    version->assign(lines.front());

  args.clear();
  base::SplitString<' '>(lines.back(), args);
  if (!args.front().empty())
    // Something went wrong.
    return false;

  return true;
}

bool DoMain(int argc, char* argv[]) {
  std::string clangd_socket_path = base::GetEnv(kEnvClangdSocket,
                                                base::kDefaultClangdSocket);

  net::NetworkService service;
  auto connection = service.Connect(clangd_socket_path);
  if (!connection)
    return true;

  proto::LocalExecute message;

  std::string current_dir = base::GetCurrentDir();
  if (current_dir.empty())
    return true;
  message.set_current_dir(current_dir);

  {
    using client::ClangFlagSet;
    auto flags = message.mutable_cc_flags();
    auto version = flags->mutable_compiler()->mutable_version();
    ClangFlagSet::StringList args;
    base::Process process(base::GetEnv(kEnvClangdCxx));
    process.AppendArg("-###").AppendArg(argv + 1, argv + argc);
    if (!process.Run(10) ||
        !ParseClangOutput(process.stderr(), version, args) ||
        ClangFlagSet::ProcessFlags(args, flags) != ClangFlagSet::COMPILE)
      return true;
  }

  {
    using client::ClangFlagSet;
    auto flags = message.mutable_pp_flags();
    auto version = flags->mutable_compiler()->mutable_version();
    ClangFlagSet::StringList args;
    base::Process process(base::GetEnv(kEnvClangdCxx));
    process.AppendArg("-###").AppendArg("-E").AppendArg(argv + 1, argv + argc);
    if (!process.Run(10) ||
        !ParseClangOutput(process.stderr(), version, args) ||
        ClangFlagSet::ProcessFlags(args, flags) != ClangFlagSet::PREPROCESS)
      return true;
  }

  proto::Status status;
  if (!connection->SendSync(message, &status)) {
    std::cerr << "Failed to send message: " << status.description()
              << std::endl;
    return true;
  }

  net::Connection::Message top_message;
  if (!connection->ReadSync(&top_message, &status)) {
    std::cerr << "Failed to read message: " << status.description()
              << std::endl;
    return true;
  }

  if (!top_message.HasExtension(proto::Status::status))
    return true;

  status = top_message.GetExtension(proto::Status::status);
  if (status.code() != proto::Status::EXECUTION &&
      status.code() != proto::Status::OK)
    return true;

  if (status.code() == proto::Status::EXECUTION)
    std::cerr << status.description();

  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  // TODO: Ignore SIGPIPE, to prevent application crash.

  /*
   * Use separate |DoMain| function to make sure that all local objects get
   * destructed before the invokation of |exec|.
   * Do not use global objects!
   */
  if (DoMain(argc, argv)) {
    return ExecuteLocally(argv);
  }
  return 0;
}
