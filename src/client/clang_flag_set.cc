#include "client/clang_flag_set.h"

#include "base/c_utils.h"
#include "proto/remote.pb.h"

namespace {

const char* kEnvTempDir = "TMPDIR";
const char* kDefaultTempDir = "/tmp";

}  // namespace

namespace dist_clang {
namespace client {

// static
ClangFlagSet::Action ClangFlagSet::ProcessFlags(StringList& flags,
                                                proto::Flags* message) {
  Action action(UNKNOWN);
  std::string temp_dir = base::GetEnv(kEnvTempDir, kDefaultTempDir);

  // Escape from double-quotes.
  for(auto it = flags.begin(); it != flags.end();
      it->empty() ? it = flags.erase(it) : ++it) {
    std::string& flag = *it;
    if (flag[0] == '"')
      flag = it->substr(1, flag.size() - 2);
  }

  // First non-empty argument is a path to executable.
  message->mutable_compiler()->set_path(flags.front());
  flags.pop_front();

  // Last argument is an input path.
  message->set_input(flags.back());
  flags.pop_back();

  for (auto it = flags.begin(); it != flags.end(); ++it) {
    std::string& flag = *it;

    if (flag == "-dynamic-linker") {
      action = LINK;
      message->add_other(flag);
    }
    else if (flag == "-emit-obj") {
      action = COMPILE;
      message->add_other(flag);
    }
    else if (flag == "-E") {
      action = PREPROCESS;
      message->add_other(flag);
    }
    else if (flag == "-dependency-file") {
      message->add_dependenies(flag);
      message->add_dependenies(*(++it));
    }
    else if (flag == "-MF") {
      message->add_dependenies(flag);
      message->add_dependenies(*(++it));
    }
    else if (flag == "-MMD") {
      message->add_dependenies(flag);
    }
    else if (flag == "-o") {
      ++it;
      if (it->find(temp_dir) != std::string::npos) {
        action = UNKNOWN;
        break;
      }
      else {
        message->set_output(*it);
      }
    }
    else {
      message->add_other(flag);
    }
  }

  return action;
}

}  // namespace client
}  // namespace dist_clang
