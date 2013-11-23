#include "client/clang.h"

#include "base/c_utils.h"
#include "base/constants.h"

#include <iostream>

#include <signal.h>

using namespace dist_clang;

namespace {

int ExecuteLocally(char* argv[]) {
  std::string clangd_cxx_path = base::GetEnv(base::kEnvClangdCxx);
  if (clangd_cxx_path.empty()) {
    std::cerr << "Provide real clang++ compiler path via "
              << base::kEnvClangdCxx << std::endl;
    return 1;
  }

  std::cout << "Running locally." << std::endl;

  if (execv(clangd_cxx_path.c_str(), argv) == -1) {
    std::cerr << "Local execution failed: " << strerror(errno) << std::endl;
    return 1;
  }

  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invokation of |exec|. Do not use global
  //         objects!
  if (client::DoMain(argc, argv)) {
    return ExecuteLocally(argv);
  }
  return 0;
}
