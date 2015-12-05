#pragma once

#include <base/aliases.h>

#include <daemon/configuration.pb.h>

namespace dist_clang {
namespace daemon {

class Configuration {
 public:
  Configuration(int argc, char* argv[]);
  Configuration(const proto::Configuration& config) : config_(config) {}
  ~Configuration();

  inline const proto::Configuration& config() const { return config_; }
  inline const bool daemonize() const { return daemonize_; }

 private:
  proto::Configuration config_;
  bool daemonize_ = false;
};

}  // namespace daemon
}  // namespace dist_clang
