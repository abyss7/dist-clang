#include "base/test_process.h"

namespace dist_clang {
namespace base {

std::unique_ptr<Process> TestProcess::Factory::Create(
    const std::string& exec_path,
    const std::string& cwd_path) {
  auto new_t = new TestProcess(exec_path, cwd_path);
  on_create_(new_t);
  return std::unique_ptr<Process>(new_t);
}

TestProcess::TestProcess(const std::string &exec_path,
                         const std::string &cwd_path)
  : Process(exec_path, cwd_path) {
}

bool TestProcess::Run(unsigned sec_timeout, std::string *error) {
  if (run_attempts_) {
    (*run_attempts_)++;
  }

  return on_run_(sec_timeout, std::string(), error);
}

bool TestProcess::Run(unsigned sec_timeout, const std::string &input,
                      std::string *error) {
  if (run_attempts_) {
    (*run_attempts_)++;
  }

  return on_run_(sec_timeout, input, error);
}

}  // namespace base
}  // namespace dist_clang
