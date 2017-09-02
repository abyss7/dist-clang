#include <base/constants.h>
#include <base/file/file.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>
#include <cache/file_cache.h>
#include <cache/manifest.pb.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace cache {

TEST(FileCacheMigratorTest, Version_0_to_1_Simple) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{"12345678901234567890123456789012-12345678-00000001"_l};
  FileCache cache(temp_dir);
  const auto common_prefix = cache.CommonPath(hash);
  const auto manifest_path = AppendExtension(common_prefix, base::kExtManifest);
  const auto object_path = AppendExtension(common_prefix, base::kExtObject);
  const auto deps_path = AppendExtension(common_prefix, base::kExtDeps);

  ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));
  ASSERT_TRUE(base::File::Write(object_path, "12345"_l));
  ASSERT_TRUE(base::File::Write(deps_path, "12345"_l));

  proto::Manifest manifest;

  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  manifest.Clear();
  EXPECT_TRUE(cache.Migrate(hash, 1));
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

TEST(FileCacheMigratorTest, Version_0_to_1_Direct) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{"12345678901234567890123456789012-12345678-00000001"_l};
  FileCache cache(temp_dir);
  const auto manifest_path =
      AppendExtension(cache.CommonPath(hash), base::kExtManifest);

  proto::Manifest manifest;
  manifest.add_headers()->assign("test.h");

  ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));
  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  manifest.Clear();
  EXPECT_TRUE(cache.Migrate(hash, 1));
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

TEST(FileCacheMigratorTest, Version_1_to_2_Simple) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{"12345678901234567890123456789012-12345678-00000001"_l};
  FileCache cache(temp_dir);
  const auto common_prefix = cache.CommonPath(hash);
  const auto manifest_path = AppendExtension(common_prefix, base::kExtManifest);
  const auto object_path = AppendExtension(common_prefix, base::kExtObject);

  ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));
  ASSERT_TRUE(base::File::Write(object_path, "12345"_l));

  proto::Manifest manifest;
  manifest.set_version(1);
  manifest.mutable_v1()->set_obj(true);

  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  manifest.Clear();
  EXPECT_TRUE(cache.Migrate(hash, 2));
  ASSERT_TRUE(base::LoadFromFile(manifest_path, &manifest));

  EXPECT_EQ(1u, manifest.version());
  EXPECT_EQ(proto::Manifest::kV1, manifest.CacheType_case());
  EXPECT_TRUE(manifest.has_v1());
  EXPECT_EQ(false, manifest.v1().snappy());
  EXPECT_EQ(false, manifest.v1().err());
  EXPECT_EQ(true, manifest.v1().obj());
  EXPECT_EQ(false, manifest.v1().dep());
}

TEST(FileCacheMigratorTest, Version_1_to_2_Direct) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{"12345678901234567890123456789012-12345678-00000001"_l};
  FileCache cache(temp_dir);
  const auto manifest_path =
      AppendExtension(cache.CommonPath(hash), base::kExtManifest);

  proto::Manifest manifest;
  manifest.set_version(1);
  manifest.mutable_direct()->add_headers()->assign("text.h");

  ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));
  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
  manifest.Clear();
  EXPECT_FALSE(cache.Migrate(hash, 2));
}

}  // namespace cache
}  // namespace dist_clang
