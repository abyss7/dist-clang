#pragma once

#include <base/types.h>
#include <client/configuration.pb.h>

namespace dist_clang {
namespace client {

class Configuration {
 public:
  Configuration();
  Configuration(const proto::Configuration& config) : config_(config) {}

  inline const proto::Configuration& config() const { return config_; }

  // For the user - the log isn't set up yet, when the config is read.
  inline const List<String>& warnings() const { return warnings_; }

 private:
  proto::Configuration config_;
  List<String> warnings_;
};

// The options of the old configs, that do nothing now - comma-separated.
String UnsupportedOptions(const proto::Configuration& config);

}  // namespace client
}  // namespace dist_clang
