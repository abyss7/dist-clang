#include "daemon/daemon.h"

#include "gtest/gtest.h"
#include "proto/config.pb.h"

namespace dist_clang {
namespace daemon {

TEST(DaemonConfigurationTest, InvalidConfig) {
  proto::Configuration config;
  config.mutable_local()->set_port(1);

  daemon::Configuration configuration(config);
  daemon::Daemon daemon;

  ASSERT_FALSE(daemon.Initialize(configuration));
}

TEST(DaemonConfigurationTest, NotListening) {
  proto::Configuration config;
  daemon::Configuration configuration(config);
  daemon::Daemon daemon;

  ASSERT_FALSE(daemon.Initialize(configuration));
}

}  // namespace daemon
}  // namespace dist_clang
