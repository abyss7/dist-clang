#include <client/configuration.hh>

#include <base/c_utils.h>
#include <base/file_utils.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

#include STL_EXPERIMENTAL(filesystem)

namespace dist_clang {
namespace client {

TEST(ClientConfigurationTest, ConfigFileInParentDir) {
  const base::TemporaryDir tmp_dir;
  const Path temp_dir_path = tmp_dir.GetPath();
  const Path child_dir = temp_dir_path / "1";
  const Path config_path = temp_dir_path / ".distclang";

  ASSERT_TRUE(base::CreateDirectory(child_dir));

  proto::Configuration config;
  config.set_path("/usr/bin/clang");

  auto* plugin = config.add_plugins();
  plugin->set_name("test_plugin");
  plugin->set_path("test_path");
  plugin->set_os(proto::Plugin::LINUX);

  plugin = config.add_plugins();
  plugin->set_name("test_plugin2");
  plugin->set_path("test_path2");
  plugin->set_os(proto::Plugin::MACOSX);

  ASSERT_TRUE(base::SaveToFile(config_path, config));

  auto old_cwd = base::GetCurrentDir();
  ASSERT_TRUE(base::ChangeCurrentDir(child_dir));
  Configuration configuration;
  EXPECT_EQ(1, configuration.config().plugins_size());
  EXPECT_TRUE(Path(configuration.config().plugins(0).path()).is_absolute())
      << configuration.config().plugins(0).path();
  base::ChangeCurrentDir(old_cwd);
}

}  // namespace client
}  // namespace dist_clang
