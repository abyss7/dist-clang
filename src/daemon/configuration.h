#pragma once

#include <proto/config.pb.h>

namespace dist_clang {
namespace daemon {

class Configuration {
 public:
  Configuration(int argc, char* argv[]);
  Configuration(const proto::Configuration& config);

  const proto::Configuration& config() const;

 private:
  bool LoadFromFile(const std::string& config_path);

  proto::Configuration config_;
};

}  // namespace daemon
}  // namespace dist_clang
