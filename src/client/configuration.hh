#pragma once

#include <client/configuration.pb.h>

namespace dist_clang {
namespace client {

class Configuration {
 public:
  Configuration();
  Configuration(const proto::Configuration& config) : config_(config) {}

  inline const proto::Configuration& config() const { return config_; }

 private:
  proto::Configuration config_;
};

}  // namespace client
}  // namespace dist_clang
