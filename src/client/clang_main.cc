#include "client/clang.h"

#include "base/c_utils.h"
#include "base/constants.h"
#include "base/logging.h"
#include "base/string_utils.h"

#include <signal.h>

#include "base/using_log.h"

using namespace dist_clang;

namespace {

int ExecuteLocally(char* argv[]) {
  std::string clangd_cxx_path = base::GetEnv(base::kEnvClangdCxx);
  if (clangd_cxx_path.empty()) {
    LOG(FATAL) << "Provide real clang++ compiler path via "
               << base::kEnvClangdCxx;
  }

  LOG(INFO) << "Running locally.";

  if (execv(clangd_cxx_path.c_str(), argv) == -1) {
    LOG(FATAL) << "Local execution failed: " << strerror(errno);
  }

  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);

  // It's safe to use |base::Log|, since its internal static objects don't need
  // special destruction on |exec|.
  std::string clangd_log_levels = base::GetEnv(base::kEnvClangdLogLevels);
  std::string clangd_log_mark = base::GetEnv(base::kEnvClangdLogMark);
  std::list<std::string> numbers;

  base::SplitString<' '>(clangd_log_levels, numbers);
  if (numbers.size() % 2 == 0) {
    base::Log::RangeSet ranges;
    for (auto number = numbers.begin(); number != numbers.end(); ++number) {
      unsigned left = base::StringTo<unsigned>(*number++);
      unsigned right = base::StringTo<unsigned>(*number);
      ranges.emplace(right, left);
    }
    base::Log::Reset(base::StringTo<unsigned>(clangd_log_mark),
                     std::move(ranges));
  }

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invokation of |exec|. Do not use global
  //         objects!
  if (client::DoMain(argc, argv)) {
    return ExecuteLocally(argv);
  }
  return 0;
}
