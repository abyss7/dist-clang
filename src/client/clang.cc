#include "base/c_utils.h"
#include "base/constants.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "client/clang_flag_set.h"
#include "net/connection.h"
#include "net/network_service.h"
#include "proto/remote.pb.h"

#include <iostream>
#include <list>
#include <string>

using std::string;
using namespace dist_clang;

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

bool DoMain(int argc, char* argv[]) {
  string clangd_socket_path = base::GetEnv(kEnvClangdSocket,
                                           base::kDefaultClangdSocket);

  net::NetworkService service;
  auto connection = service.Connect(clangd_socket_path, nullptr);
  if (!connection)
    return true;

  base::Process split_process(base::GetEnv(kEnvClangdCxx));
  split_process
      .AppendArg("-###")
      .AppendArg(argv + 1, argv + argc);
  if (!split_process.Run(30, nullptr))
    return true;

  string current_dir = base::GetCurrentDir();
  if (current_dir.empty())
    return true;

  client::ClangFlagSet::string_list lines;
  base::SplitString<'\n'>(split_process.stderr(), lines);
  if (lines.size() != 4)
    // FIXME: we don't support composite tasks yet.
    return true;

  client::ClangFlagSet::string_list args;
  base::SplitString<' '>(lines.back(), args);
  if (!args.front().empty())
    // Something went wrong.
    return true;

  auto action = client::ClangFlagSet::ProcessFlags(args);
  if (action == client::ClangFlagSet::UNKNOWN)
    return true;

  proto::Universal top_message;
  auto message = top_message.mutable_execute();
  message->set_origin(proto::Execute::LOCAL);
  message->set_current_dir(current_dir);
  for (auto it = args.begin(); it != args.end(); ++it)
    message->add_args()->assign(*it);

  if (!connection->Send(top_message, nullptr))
    return true;

  if (!connection->Read(&top_message, nullptr))
    return true;

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
