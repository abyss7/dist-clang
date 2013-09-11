#include "client/clang_flag_set.h"

#include "base/c_utils.h"

using std::string;

namespace {

const char* kEnvTempDir = "TMPDIR";
const char* kDefaultTempDir = "/tmp";

}

namespace dist_clang {
namespace client {

// static
ClangFlagSet::Action ClangFlagSet::ProcessFlags(string_list& args) {
  Action action(UNKNOWN);
  string temp_dir = base::GetEnv(kEnvTempDir, kDefaultTempDir);

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

}  // namespace client
}  // namespace dist_clang
