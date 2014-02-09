#include "base/process.h"

namespace dist_clang {
namespace base {

Process::Process(const std::string& exec_path, const std::string& cwd_path)
  : exec_path_(exec_path), cwd_path_(cwd_path) {
}

Process& Process::AppendArg(const std::string& arg) {
  args_.push_back(arg);
  return *this;
}

}  // namespace base
}  // namespace dist_clang
