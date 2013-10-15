#include "base/process.h"

#include "proto/remote.pb.h"

#include <signal.h>

namespace dist_clang {
namespace base {

Process::Process(const std::string& exec_path, const std::string& cwd_path)
  : exec_path_(exec_path), cwd_path_(cwd_path), killed_(false) {
}

Process::Process(const proto::Flags &flags, const std::string &cwd_path)
  : Process(flags.compiler().path(), cwd_path) {
  // |flags.other()| always must go first, since they contain "-cc1" flag.
  AppendArg(flags.other().begin(), flags.other().end());
  AppendArg(flags.dependenies().begin(), flags.dependenies().end());
  for (const auto& plugin: flags.compiler().plugins()) {
    AppendArg("-load").AppendArg(plugin.path());
  }
  if (flags.has_language()) {
    AppendArg("-x").AppendArg(flags.language());
  }
  if (flags.has_output()) {
    AppendArg("-o").AppendArg(flags.output());
  }
  if (flags.has_input()) {
    AppendArg(flags.input());
  }
}

Process& Process::AppendArg(const std::string& arg) {
  args_.push_back(arg);
  return *this;
}

void Process::kill(int pid) {
  ::kill(pid, SIGTERM);
  killed_ = true;
}

}  // namespace base
}  // namespace dist_clang
