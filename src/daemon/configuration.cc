#include "daemon/configuration.h"

#include "base/constants.h"
#include "tclap/CmdLine.h"

#include <string>

using std::string;
using namespace TCLAP;

namespace dist_clang {
namespace daemon {

Configuration::Configuration(int argc, char *argv[]) {
  try {
    // TODO: use normal versioning.
    CmdLine cmd("Daemon from Clang distributed system - Clangd.", ' ', "0.1");
    ValueArg<string> socket_arg("s", "socket",
        "Path to UNIX socket to listen for local connections.",
        false, base::kDefaultClangdSocket, "path", cmd);
    cmd.parse(argc, argv);

    config_.set_socket_path(socket_arg.getValue());
  } catch (ArgException &e) {
    std::cerr << e.error() << " for argument " << e.argId() << std::endl;
    return;
  }
}

const proto::Configuration& Configuration::config() const {
  return config_;
}

}  // namespace daemon
}  // namespace dist_clang
