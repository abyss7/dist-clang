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

using std::string;
using namespace dist_clang;
using client::ClangFlagSet;

namespace {

const char* kEnvClangdSocket = "CLANGD_SOCKET_PATH";
const char* kEnvClangdCxx = "CLANGD_CXX";

int ExecuteLocally(char* argv[]) {
  string clangd_cxx_path = base::GetEnv(kEnvClangdCxx);
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

bool ParseClangOutput(const string& output,
                      ClangFlagSet::string_list& args) {
  ClangFlagSet::string_list lines;
  base::SplitString<'\n'>(output, lines);
  if (lines.size() != 4)
    // FIXME: we don't support composite tasks yet.
    return false;

  args.clear();
  base::SplitString<' '>(lines.back(), args);
  if (!args.front().empty())
    // Something went wrong.
    return false;

  return true;
}

bool DoMain(int argc, char* argv[]) {
  string clangd_socket_path = base::GetEnv(kEnvClangdSocket,
                                           base::kDefaultClangdSocket);

  net::NetworkService service;
  auto connection = service.Connect(clangd_socket_path, nullptr);
  if (!connection)
    return true;

  proto::LocalExecute message;
  ClangFlagSet::string_list args;

  string current_dir = base::GetCurrentDir();
  if (current_dir.empty())
    return true;
  message.set_current_dir(current_dir);

  // The clang output has following format:
  //
  // clang version 3.4 (http://llvm.org/git/clang.git <commit-hash1>) (http://llvm.org/git/llvm.git <commit-hash2>)
  // Target: x86_64-unknown-linux-gnu
  // Thread model: posix
  //  "/home/ilezhankin/llvm/bin/clang" "-cc1" "-triple" ...
  //
  // Pay attention to the leading space in the fourth line.

  base::Process cc_process(base::GetEnv(kEnvClangdCxx));
  cc_process.AppendArg("-###").AppendArg(argv + 1, argv + argc);
  if (!cc_process.Run(10, nullptr) ||
      !ParseClangOutput(cc_process.stderr(), args) ||
      ClangFlagSet::ProcessFlags(args, message.mutable_cc_flags()) !=
          ClangFlagSet::COMPILE)
    return true;

  base::Process pp_process(base::GetEnv(kEnvClangdCxx));
  pp_process.AppendArg("-###").AppendArg("-E").AppendArg(argv + 1, argv + argc);
  if (!pp_process.Run(10, nullptr) ||
      !ParseClangOutput(pp_process.stderr(), args) ||
      ClangFlagSet::ProcessFlags(args, message.mutable_pp_flags()) !=
          ClangFlagSet::PREPROCESS)
    return true;

  if (!connection->SendSync(message))
    return true;

  net::Connection::Message top_message;
  if (!connection->ReadSync(&top_message))
    return true;

  if (!top_message.HasExtension(proto::Error::error))
    return true;

  const proto::Error& error = top_message.GetExtension(proto::Error::error);
  if (error.code() != proto::Error::EXECUTION &&
      error.code() != proto::Error::OK)
    return true;

  if (error.code() == proto::Error::EXECUTION)
    std::cerr << error.description();

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
