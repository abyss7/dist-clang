#include <daemon/configuration.h>

#include <base/logging.h>
#include <base/protobuf_utils.h>

#include <third_party/gflags/linux/gflags.h>
#include <third_party/protobuf/exported/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

#include <fcntl.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

DEFINE_string(config, String(), "Path to the configuration file");
DEFINE_bool(daemon, false, "Daemonize after start");

Configuration::Configuration(int argc, char* argv[]) {
  gflags::SetUsageMessage("Daemon from Clang distributed system.");
  gflags::SetVersionString(VERSION);

  gflags::ParseCommandLineFlags(&argc, &argv, false);

  String error;
  if (!FLAGS_config.empty() &&
      !base::LoadFromFile(FLAGS_config, &config_, &error)) {
    LOG(ERROR) << "Failed to read configuration from " << FLAGS_config << ": "
               << error;
  }
  daemonize_ = FLAGS_daemon;
}

Configuration::~Configuration() {
  gflags::ShutDownCommandLineFlags();
}

}  // namespace daemon
}  // namespace dist_clang
