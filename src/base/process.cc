#include <base/process.h>

namespace dist_clang {
namespace base {

Process::Process(const String& exec_path, const Path& cwd_path, ui32 uid)
    : cwd_path_(cwd_path), exec_path_(exec_path), uid_(uid) {}

Process& Process::AppendArg(Immutable arg) {
  args_.push_back(arg);
  return *this;
}

Process& Process::AddEnv(const char* name, const char* value) {
  envs_.push_back(String(name) + "=" + value);
  return *this;
}

}  // namespace base
}  // namespace dist_clang
