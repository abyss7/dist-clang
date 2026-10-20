#include <daemon/configuration.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <stdlib.h>

namespace dist_clang {
namespace daemon {

TEST(DaemonConfigurationTest, SocketOfAutostart) {
  // The configuration of the user is not read.
  const base::TemporaryDir temp_dir;
  setenv("XDG_CONFIG_HOME", temp_dir.path().c_str(), 1);

  // The client passes the socket, that it has locked - the daemon must listen
  // there.
  const char* args[] = {"dist-clangd", "--autostart", "--lock-fd=3",
                        "--socket=/run/test/x.sock"};
  Configuration configuration(4, const_cast<char**>(args));
  EXPECT_EQ("/run/test/x.sock", configuration.config().emitter().socket_path());
  EXPECT_EQ(3, configuration.lock_fd());

  unsetenv("XDG_CONFIG_HOME");
}

TEST(DaemonConfigurationTest, PartialConfigOfAutostart) {
  const base::TemporaryDir temp_dir;
  setenv("XDG_CONFIG_HOME", temp_dir.path().c_str(), 1);
  std::filesystem::create_directory(temp_dir.path() / "dist-clang");
  const auto config_path = temp_dir.path() / "dist-clang" / "daemon.conf";
  const char* args[] = {"dist-clangd", "--autostart"};

  const Configuration defaults(2, const_cast<char**>(args));

  // The config changes only the size of the cache - the rest is by default.
  ASSERT_TRUE(base::File::Write(config_path, "cache { size: 1024 }"_l));
  {
    const Configuration configuration(2, const_cast<char**>(args));
    EXPECT_EQ(1024u, configuration.config().cache().size());
    EXPECT_TRUE(configuration.config().cache().direct());
    EXPECT_EQ(defaults.config().cache().path(),
              configuration.config().cache().path());
    EXPECT_EQ(defaults.config().emitter().threads(),
              configuration.config().emitter().threads());
    EXPECT_EQ(defaults.config().emitter().socket_path(),
              configuration.config().emitter().socket_path());
  }

  // The broken config doesn't stop the builds.
  ASSERT_TRUE(base::File::Write(config_path, "cache { size: "_l));
  {
    const Configuration configuration(2, const_cast<char**>(args));
    EXPECT_EQ(defaults.config().SerializeAsString(),
              configuration.config().SerializeAsString());
  }

  unsetenv("XDG_CONFIG_HOME");
}

}  // namespace daemon
}  // namespace dist_clang
