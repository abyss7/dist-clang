#pragma once

#include "proto/config.pb.h"

namespace dist_clang {
namespace daemon {

class Configuration {
  public:
    Configuration(int argc, char* argv[]);

    const proto::Configuration& config() const;

  private:
    proto::Configuration config_;
};

}  // namespace daemon
}  // namespace dist_clang
