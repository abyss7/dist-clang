#include <client/clang.h>

#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <base/string_utils.h>

#include <signal.h>

#include <base/using_log.h>

using namespace dist_clang;

namespace {

int ExecuteLocally(char* argv[], const String& clangd_cxx_path) {
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
  String clangd_log_levels = base::GetEnv(base::kEnvClangdLogLevels);

  if (!clangd_log_levels.empty()) {
    String clangd_log_mark = base::GetEnv(base::kEnvClangdLogMark);
    List<String> numbers;

    base::SplitString<' '>(clangd_log_levels, numbers);
    if (numbers.size() % 2 == 0) {
      base::Log::RangeSet ranges;
      for (auto number = numbers.begin(); number != numbers.end(); ++number) {
        ui32 left = base::StringTo<ui32>(*number++);
        ui32 right = base::StringTo<ui32>(*number);
        ranges.emplace(right, left);
      }
      base::Log::Reset(base::StringTo<ui32>(clangd_log_mark),
                       std::move(ranges));
    }
  }

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invokation of |exec|. Do not use global
  //         objects!
  String clang_path = base::GetEnv(base::kEnvClangdCxx);
  String socket_path =
      base::GetEnv(base::kEnvClangdSocket, base::kDefaultClangdSocket);
  if (client::DoMain(argc, argv, socket_path, clang_path)) {
    return ExecuteLocally(argv, clang_path);
  }
  return 0;
}
