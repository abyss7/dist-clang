#include <client/configuration.hh>

#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <sys/stat.h>

namespace dist_clang {
namespace client {

TEST(ClientConfigurationTest, ConfigFileInParentDir) {
  const base::TemporaryDir temp_dir;
  const auto child_dir = temp_dir.path() / "1";
  ASSERT_TRUE(base::CreateDirectory(child_dir));

  // The relative path is relative to the directory of the config.
  proto::Configuration config;
  config.set_path("bin/clang");
  ASSERT_TRUE(base::SaveToFile(temp_dir.path() / ".distclang", config));
  unsetenv("DC_CLANG_PATH");

  const auto old_cwd = base::GetCurrentDir();
  ASSERT_TRUE(base::ChangeCurrentDir(child_dir));
  {
    Configuration configuration;
    EXPECT_EQ(temp_dir.path() / "bin" / "clang",
              Path(configuration.config().path()));
  }

  // The environment prevails.
  setenv("DC_CLANG_PATH", "/usr/bin/clang", 1);
  {
    Configuration configuration;
    EXPECT_EQ("/usr/bin/clang", configuration.config().path());
  }
  unsetenv("DC_CLANG_PATH");
  base::ChangeCurrentDir(old_cwd);
}

TEST(ClientConfigurationTest, UnsupportedOptions) {
  proto::Configuration config;
  config.set_path("/usr/bin/clang");
  config.set_disabled(true);
  EXPECT_EQ("", UnsupportedOptions(config));

  // The old configs still parse, but these options do nothing now.
  auto* plugin = config.add_plugins();
  plugin->set_name("test_plugin");
  plugin->set_path("test_path");
  config.set_version("3.5");
  config.set_rewrite_includes(true);
  EXPECT_EQ("plugins, version, rewrite_includes", UnsupportedOptions(config));

  // The user is warned.
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::SaveToFile(temp_dir.path() / ".distclang", config));
  const auto old_cwd = base::GetCurrentDir();
  ASSERT_TRUE(base::ChangeCurrentDir(temp_dir));
  Configuration configuration;
  base::ChangeCurrentDir(old_cwd);
  ASSERT_EQ(1u, configuration.warnings().size());
  EXPECT_NE(String::npos, configuration.warnings().front().find(
                              "ignored: plugins, version, rewrite_includes"))
      << configuration.warnings().front();
}

TEST(ClientConfigurationTest, ConfigFileWithoutPath) {
  // The config may only disable the client - the compiler is found in PATH.
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / ".distclang",
                                "disabled: true"_l));
  unsetenv("DC_DISABLED");

  const auto old_cwd = base::GetCurrentDir();
  ASSERT_TRUE(base::ChangeCurrentDir(temp_dir));
  Configuration configuration;
  EXPECT_TRUE(configuration.config().disabled());
  EXPECT_TRUE(configuration.warnings().empty());
  base::ChangeCurrentDir(old_cwd);
}

TEST(ClientConfigurationTest, DontFindItself) {
  // A symlink "clang" to the client itself in PATH isn't the real compiler:
  // it would run itself forever.
  const base::TemporaryDir temp_dir;
  String self;
  ASSERT_TRUE(base::GetSelfExecutable(self));
  std::filesystem::create_symlink(std::filesystem::canonical(self),
                                  temp_dir.path() / "clang");
  const String old_path = String(base::GetEnv("PATH"_l));
  setenv("PATH", (temp_dir.path().string() + ":" + old_path).c_str(), 1);
  unsetenv("DC_CLANG_PATH");

  const auto old_cwd = base::GetCurrentDir();
  ASSERT_TRUE(base::ChangeCurrentDir(temp_dir));
  Configuration configuration;
  EXPECT_NE(temp_dir.path() / "clang", Path(configuration.CompilerPath()));
  base::ChangeCurrentDir(old_cwd);
  setenv("PATH", old_path.c_str(), 1);
}

TEST(ClientConfigurationTest, FindCompilerOnDemand) {
  // The compiler is looked up in PATH only, when it's needed - not for the
  // launcher.
  const base::TemporaryDir temp_dir;
  const auto clang_path = temp_dir.path() / "clang";
  ASSERT_TRUE(base::File::Write(clang_path, "#!/bin/sh\n"_l));
  ASSERT_EQ(0, chmod(clang_path.c_str(), 0755));
  unsetenv("DC_CLANG_PATH");
  const String old_path = String(base::GetEnv("PATH"_l));
  setenv("PATH", "", 1);

  Configuration configuration;
  setenv("PATH", temp_dir.path().c_str(), 1);
  EXPECT_EQ(clang_path, Path(configuration.CompilerPath()));
  setenv("PATH", old_path.c_str(), 1);
}

}  // namespace client
}  // namespace dist_clang
