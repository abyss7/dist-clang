#include <client/clang.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace client {

TEST(ClangTest, TrustOnlyOwnOrRootDaemon) {
  EXPECT_TRUE(IsTrustedDaemon(1000, 1000));
  EXPECT_TRUE(IsTrustedDaemon(1000, 0));
  EXPECT_FALSE(IsTrustedDaemon(1000, 1001));
  EXPECT_TRUE(IsTrustedDaemon(0, 0));
  EXPECT_FALSE(IsTrustedDaemon(0, 1000));
}

TEST(ClangTest, DaemonStatusText) {
  perf::proto::DaemonStatus daemon;
  daemon.set_version("1.2.3");
  daemon.set_cache_path("/cache");
  EXPECT_EQ(
      "Daemon 1.2.3\n"
      "  Config  none - the defaults\n"
      "  Cache   /cache\n",
      DaemonStatusText(daemon));

  // Why the config and the cache aren't used.
  daemon.set_config_path("/conf/daemon.conf");
  daemon.set_config_error("line 1: broken");
  daemon.clear_cache_path();
  daemon.set_cache_error("the cache /cache is used by another daemon, pid 1");
  EXPECT_EQ(
      "Daemon 1.2.3\n"
      "  Config  /conf/daemon.conf - not used, the defaults are: line 1: "
      "broken\n"
      "  Cache   off - the cache /cache is used by another daemon, pid 1\n",
      DaemonStatusText(daemon));

  daemon.clear_config_error();
  EXPECT_NE(String::npos,
            DaemonStatusText(daemon).find("  Config  /conf/daemon.conf\n"));
}

}  // namespace client
}  // namespace dist_clang
