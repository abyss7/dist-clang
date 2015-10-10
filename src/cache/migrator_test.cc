#include <cache/migrator.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>
#include <cache/manifest.pb.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace cache {

TEST(MigratorTest, Version_0_to_1_Simple) {
  const base::TemporaryDir tmp_dir;
  const String common_path = String(tmp_dir) + "/hash";
  const String manifest_path = common_path + ".manifest";

  ASSERT_TRUE(base::File::Write(common_path + ".o", "12345"_l));
  ASSERT_TRUE(base::File::Write(common_path + ".d", "12345"_l));

  proto::Manifest manifest;

  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  EXPECT_TRUE(Migrate(common_path));
  ASSERT_TRUE(base::LoadFromFile(manifest_path, &manifest));

  EXPECT_EQ(1u, manifest.version());
  EXPECT_EQ(proto::Manifest::kV1, manifest.CacheType_case());
  EXPECT_TRUE(manifest.has_v1());
  EXPECT_EQ(false, manifest.v1().snappy());
  EXPECT_EQ(false, manifest.v1().err());
  EXPECT_EQ(true, manifest.v1().obj());
  EXPECT_EQ(true, manifest.v1().dep());
  EXPECT_EQ(10u, manifest.v1().size());
  EXPECT_FALSE(manifest.has_direct());
  EXPECT_FALSE(manifest.headers_size());
  EXPECT_FALSE(manifest.has_snappy());
  EXPECT_FALSE(manifest.has_stderr());
  EXPECT_FALSE(manifest.has_object());
  EXPECT_FALSE(manifest.has_deps());
}

TEST(MigratorTest, Version_0_to_1_Direct) {
  const base::TemporaryDir tmp_dir;
  const String common_path = String(tmp_dir) + "/hash";
  const String manifest_path = common_path + ".manifest";

  proto::Manifest manifest;
  manifest.add_headers()->assign("test.h");

  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  EXPECT_TRUE(Migrate(common_path));
  ASSERT_TRUE(base::LoadFromFile(manifest_path, &manifest));

  EXPECT_EQ(1u, manifest.version());
  EXPECT_EQ(proto::Manifest::kDirect, manifest.CacheType_case());
  EXPECT_TRUE(manifest.has_direct());
  EXPECT_EQ(1, manifest.direct().headers().size());
  EXPECT_EQ("test.h", manifest.direct().headers(0));
  EXPECT_FALSE(manifest.has_v1());
  EXPECT_FALSE(manifest.headers_size());
  EXPECT_FALSE(manifest.has_snappy());
  EXPECT_FALSE(manifest.has_stderr());
  EXPECT_FALSE(manifest.has_object());
  EXPECT_FALSE(manifest.has_deps());
}

}  // namespace cache
}  // namespace dist_clang
