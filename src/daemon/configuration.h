#pragma once

#include <base/types.h>

#include <daemon/configuration.pb.h>

namespace dist_clang {
namespace daemon {

class Configuration {
 public:
  Configuration(int argc, char* argv[]);
  Configuration(const proto::Configuration& config) : config_(config) {}

  inline const proto::Configuration& config() const { return config_; }
  inline bool daemonize() const { return daemonize_; }
  inline bool autostart() const { return autostart_; }
  inline int lock_fd() const { return lock_fd_; }
  // The read file - empty, if only the defaults are used.
  inline const String& config_path() const { return config_path_; }
  // Why the file isn't used.
  inline const String& config_error() const { return config_error_; }

 private:
  proto::Configuration config_;
  bool daemonize_ = false;
  bool autostart_ = false;
  int lock_fd_ = -1;
  String config_path_, config_error_;
};

}  // namespace daemon
}  // namespace dist_clang
