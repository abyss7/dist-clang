#include <daemon/configuration.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <stdlib.h>

namespace dist_clang {
namespace daemon {

namespace {

bool Logs(const base::proto::Verbosity& verbosity, ui32 level) {
  for (const auto& range : verbosity.levels()) {
    if (range.left() <= level && level <= range.right()) {
      return true;
    }
  }
  return false;
}

}  // namespace

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
  // Only the defaults - for "dist-clang --stats".
  EXPECT_TRUE(defaults.config_path().empty());
  EXPECT_TRUE(defaults.config_error().empty());

  // The config changes only the size of the cache - the rest is by default.
  ASSERT_TRUE(base::File::Write(config_path, "cache { size: 1024 }"_l));
  {
    const Configuration configuration(2, const_cast<char**>(args));
    EXPECT_EQ(1024u, configuration.config().cache().size());
    EXPECT_EQ(config_path, configuration.config_path());
    EXPECT_TRUE(configuration.config_error().empty());
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
    EXPECT_EQ(config_path, configuration.config_path());
    // Where the file is broken.
    EXPECT_NE(String::npos, configuration.config_error().find("line 1"))
        << configuration.config_error();
  }

  // The unknown field.
  ASSERT_TRUE(base::File::Write(config_path, "cache {\n  sise: 1\n}"_l));
  {
    const Configuration configuration(2, const_cast<char**>(args));
    EXPECT_NE(String::npos, configuration.config_error().find("line 2"))
        << configuration.config_error();
    EXPECT_NE(String::npos, configuration.config_error().find("sise"))
        << configuration.config_error();
  }

  unsetenv("XDG_CONFIG_HOME");
}

TEST(DaemonConfigurationTest, DefaultVerbosity) {
  const base::TemporaryDir temp_dir;
  setenv("XDG_CONFIG_HOME", temp_dir.path().c_str(), 1);
  std::filesystem::create_directory(temp_dir.path() / "dist-clang");
  const auto config_path = temp_dir.path() / "dist-clang" / "daemon.conf";
  const char* args[] = {"dist-clangd", "--autostart"};

  // The errors and the warnings of everything are logged, but not the
  // messages about every compilation.
  {
    const Configuration configuration(2, const_cast<char**>(args));
    const auto& verbosity = configuration.config().verbosity();
    for (ui32 level : {0u, 10u, 20u, 110u, 120u, 210u, 220u}) {
      EXPECT_TRUE(Logs(verbosity, level)) << level;
    }
    for (ui32 level : {30u, 40u, 130u, 140u, 230u, 240u}) {
      EXPECT_FALSE(Logs(verbosity, level)) << level;
    }
  }

  // The verbosity of the user replaces the default one.
  ASSERT_TRUE(base::File::Write(
      config_path, "verbosity { levels { left: 0 right: 40 } }"_l));
  {
    const Configuration configuration(2, const_cast<char**>(args));
    const auto& verbosity = configuration.config().verbosity();
    EXPECT_TRUE(Logs(verbosity, 40));
    EXPECT_FALSE(Logs(verbosity, 110));
  }

  unsetenv("XDG_CONFIG_HOME");
}

}  // namespace daemon
}  // namespace dist_clang
