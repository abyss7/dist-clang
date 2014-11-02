#include <daemon/configuration.h>

#include <base/constants.h>
#include <base/logging.h>
#include <base/string_utils.h>

#include <third_party/protobuf/exported/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/google/protobuf/text_format.h>
#include <third_party/tclap/exported/include/tclap/CmdLine.h>

#include <fcntl.h>

#include <base/using_log.h>

using namespace TCLAP;

namespace dist_clang {
namespace daemon {

Configuration::Configuration(int argc, char* argv[]) {
  try {
    CmdLine cmd("Daemon from Clang distributed system - Clangd.", ' ', VERSION);
    ValueArg<String> config_arg("", "config", "Path to the configuration file.",
                                false, String(), "path", cmd);
    cmd.parse(argc, argv);

    if (config_arg.isSet() && !LoadFromFile(config_arg.getValue())) {
      LOG(ERROR) << "Failed to read config file: " << config_arg.getValue();
    }
  } catch (ArgException& e) {
    LOG(ERROR) << e.argId() << std::endl << e.error();
    return;
  }
}

Configuration::Configuration(const proto::Configuration& config)
    : config_(config) {
}

bool Configuration::LoadFromFile(const String& config_path) {
  auto fd = open(config_path.c_str(), O_RDONLY);
  if (fd == -1) {
    return false;
  }
  google::protobuf::io::FileInputStream input(fd);
  input.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&input, &config_)) {
    config_.Clear();
    return false;
  }
  return true;
}

const proto::Configuration& Configuration::config() const {
  return config_;
}

}  // namespace daemon
}  // namespace dist_clang
