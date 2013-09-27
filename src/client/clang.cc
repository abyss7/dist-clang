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
  auto connection = service.Connect(clangd_socket_path, nullptr);
  if (!connection)
    return true;

  proto::LocalExecute message;
  client::ClangFlagSet::StringList args;
  std::string* version;

  std::string current_dir = base::GetCurrentDir();
  if (current_dir.empty())
    return true;
  message.set_current_dir(current_dir);

  version = message.mutable_cc_flags()->mutable_compiler()->mutable_version();
  base::Process cc_process(base::GetEnv(kEnvClangdCxx));
  cc_process.AppendArg("-###").AppendArg(argv + 1, argv + argc);
  if (!cc_process.Run(10, nullptr) ||
      !ParseClangOutput(cc_process.stderr(), version, args) ||
      client::ClangFlagSet::ProcessFlags(args, message.mutable_cc_flags())
          != client::ClangFlagSet::COMPILE)
    return true;

  version = message.mutable_pp_flags()->mutable_compiler()->mutable_version();
  base::Process pp_process(base::GetEnv(kEnvClangdCxx));
  pp_process.AppendArg("-###").AppendArg("-E").AppendArg(argv + 1, argv + argc);
  if (!pp_process.Run(10, nullptr) ||
      !ParseClangOutput(pp_process.stderr(), version, args) ||
      client::ClangFlagSet::ProcessFlags(args, message.mutable_pp_flags())
          != client::ClangFlagSet::PREPROCESS)
    return true;

  if (!connection->SendSync(message))
    return true;

  net::Connection::Message top_message;
  if (!connection->ReadSync(&top_message))
    return true;

  if (!top_message.HasExtension(proto::Status::status))
    return true;

  const proto::Status& status = top_message.GetExtension(proto::Status::status);
  if (status.code() != proto::Status::EXECUTION &&
      status.code() != proto::Status::OK)
    return true;

  if (status.code() == proto::Status::EXECUTION)
    std::cerr << status.description();

  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  /*
   * Use separate |DoMain| function to make sure that all local objects get
   * destructed before the invokation of |exec|.
   * Do not use global objects!
   */
  if (DoMain(argc, argv))
    return ExecuteLocally(argv);

  return 0;
}
