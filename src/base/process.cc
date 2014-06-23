#include <base/process.h>

namespace dist_clang {
namespace base {

Process::Process(const String& exec_path, const String& cwd_path, ui32 uid)
    : exec_path_(exec_path), cwd_path_(cwd_path), uid_(uid) {}

Process& Process::AppendArg(const String& arg) {
  args_.push_back(arg);
  return *this;
}

Process& Process::AddEnv(const char* name, const String& value) {
  envs_.push_back(String(name) + "=" + value);
  return *this;
}

}  // namespace base
}  // namespace dist_clang
