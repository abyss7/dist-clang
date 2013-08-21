#include "base/c_utils.h"
#include "base/process.h"
#include "base/string_utils.h"
#include "client/client_unix.h"
#include "proto/remote.pb.h"

#include <iostream>
#include <list>
#include <string>

namespace {

using std::list;
using std::string;

typedef list<string> string_list;

const char* kEnvClangdSocket = "CLANGD_SOCKET_PATH";
const char* kEnvClangdCxx = "CLANGD_CXX";
const char* kEnvTempDir = "TMPDIR";
const char* kDefaultClangdSocket = "/tmp/clangd.socket";
const char* kDefaultTempDir = "/tmp";

int ExecuteLocally(char* argv[]) {
  string clangd_cxx_path = GetEnv(kEnvClangdCxx);
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

class ClangFlagSet {
  public:
    enum Action {
      COMPILE,
      LINK,
      UNKNOWN,
    };
    static Action ProcessFlags(string_list& args) {
      Action action(UNKNOWN);
      string temp_dir = GetEnv(kEnvTempDir, kDefaultTempDir);

      // Escape from double-quotes.
      for(auto it = args.begin(); it != args.end();
          it->empty() ? it = args.erase(it) : ++it) {
        string& flag = *it;
        if (flag[0] == '"')
          flag = it->substr(1, flag.size() - 2);
      }

      for (auto it = args.begin(); it != args.end(); ++it) {
        string& flag = *it;

        if (flag == "-dynamic-linker") {
          action = LINK;
        }
        else if (flag == "-emit-obj") {
          action = COMPILE;
        }
        else if (flag == "-o") {
          ++it;
          if (it->find(temp_dir) != string::npos) {
            action = UNKNOWN;
            break;
          }
        }
      }

      return action;
    }
};

bool DoMain(int argc, char* argv[]) {
  string clangd_socket_path = GetEnv(kEnvClangdSocket, kDefaultClangdSocket);

  UnixClient local_socket;
  if (!local_socket.Connect(clangd_socket_path, nullptr))
    return true;

  Process split_process(GetEnv(kEnvClangdCxx));
  split_process
      .AppendArg("-###")
      .AppendArg(argv + 1, argv + argc);
  if (!split_process.Run(30, nullptr))
    return true;

  string current_dir = GetCurrentDir();
  if (current_dir.empty())
    return true;

  string_list lines;
  SplitString<'\n'>(split_process.stderr(), lines);
  if (lines.size() != 4)
    // FIXME: we don't support composite tasks yet.
    return true;

  string_list args;
  SplitString<' '>(lines.back(), args);
  if (!args.front().empty())
    // Something went wrong.
    return true;

  ClangFlagSet::Action action = ClangFlagSet::ProcessFlags(args);
  if (action == ClangFlagSet::UNKNOWN)
    return true;

  clangd::LocalExecution message;
  message.set_current_dir(current_dir);
  for (auto it = args.begin(); it != args.end(); ++it)
    message.add_args()->assign(*it);

  if (!local_socket.Send(message, nullptr))
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
