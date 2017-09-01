#include <base/test_process.h>

namespace dist_clang {
namespace base {

UniquePtr<Process> TestProcess::Factory::Create(const Path& exec_path,
                                                const Path& cwd_path,
                                                ui32 uid) {
  auto new_t = new TestProcess(exec_path, cwd_path, uid);
  on_create_(new_t);
  return UniquePtr<Process>(new_t);
}

TestProcess::TestProcess(const Path& exec_path, const Path& cwd_path, ui32 uid)
    : Process(exec_path, cwd_path, uid) {}

bool TestProcess::Run(ui16 sec_timeout, String* error) {
  if (run_attempts_) {
    (*run_attempts_)++;
  }

  return on_run_(sec_timeout, String(), error);
}

bool TestProcess::Run(ui16 sec_timeout, Immutable input, String* error) {
  if (run_attempts_) {
    (*run_attempts_)++;
  }

  return on_run_(sec_timeout, input, error);
}

String TestProcess::PrintArgs() const {
  String result;

  for (const auto& arg : args_) {
    result += arg + " "_l;
  }

  return result;
}

}  // namespace base
}  // namespace dist_clang
