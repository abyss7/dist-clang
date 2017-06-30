#include <base/file/file.h>

#include <base/const_string.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

#include STL(fstream)
#include STL_EXPERIMENTAL(filesystem)
#include STL(system_error)

namespace dist_clang {
namespace base {

TEST(FileTest, Read) {
  const auto expected_content = "All your base are belong to us"_l;
  const base::TemporaryDir temp_dir;
  const Path temp_dir_path = temp_dir.GetPath();
  const Path file_path = temp_dir_path / "file";

  {
    std::ofstream file(file_path, std::ios_base::out | std::ios_base::trunc);
    ASSERT_TRUE(file.good());
    file << expected_content;
  }

  Immutable content(true);
  // |content| is assignable since we use it many times during test.

  String error;
  EXPECT_TRUE(File::Read(file_path, &content, &error)) << error;
  EXPECT_EQ(expected_content, content);

  File file(file_path);
  EXPECT_TRUE(file.Read(&content, &error)) << error;
  EXPECT_EQ(expected_content, content);

  // Can't open directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Read(&content));
}

TEST(FileTest, Write) {
  const auto expected_content = "All your base are belong to us"_l;
  const base::TemporaryDir temp_dir;
  const Path file_path = temp_dir.GetPath() / "file";

  String error;
  EXPECT_TRUE(File::Write(file_path, expected_content, &error)) << error;

  char content[expected_content.size()];
  std::ifstream file(file_path);
  ASSERT_TRUE(file.good());
  file.read(content, expected_content.size());

  EXPECT_EQ(expected_content, String(content, expected_content.size()));
  // Can't write to directory.
  EXPECT_FALSE(File::Write(temp_dir, expected_content));
}

TEST(FileTest, Size) {
  const base::TemporaryDir temp_dir;
  const Path temp_dir_path = temp_dir.GetPath();
  const Path file_path = temp_dir_path / "file";
  const String content = "1234567890";

  {
    std::ofstream file(file_path, std::ios_base::out | std::ios_base::trunc);
    ASSERT_TRUE(file.good());
    file << content;
  }
  EXPECT_EQ(content.size(), File::Size(file_path));

  File file(file_path);
  EXPECT_EQ(content.size(), file.Size());

  // Can't get size of directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Size());
}

TEST(FileTest, Hash) {
  const auto content = "All your base are belong to us"_l;
  const auto expected_hash = "c9e92e37df1e856cbd0abffe104225b8"_l;
  const base::TemporaryDir temp_dir;
  const Path temp_dir_path = temp_dir.GetPath();
  const String file_path = temp_dir_path / "file";

  {
    std::ofstream file(file_path, std::ios_base::out | std::ios_base::trunc);
    ASSERT_TRUE(file.good());
    file << content;
  }

  Immutable hash(true);
  // |hash| is assignable since we use it many times during test.

  String error;
  EXPECT_TRUE(File::Hash(file_path, &hash, List<Literal>(), &error)) << error;
  EXPECT_EQ(expected_hash, hash);
  EXPECT_FALSE(File::Hash(file_path, &hash, {"belong"_l}, &error));
  EXPECT_TRUE(File::Hash(file_path, &hash, {"bycicle"_l}, &error)) << error;
  EXPECT_EQ(expected_hash, hash);

  File file(file_path);
  EXPECT_TRUE(file.Hash(&hash, List<Literal>(), &error)) << error;
  EXPECT_EQ(expected_hash, hash);
  EXPECT_FALSE(file.Hash(&hash, {"belong"_l}, &error));
  EXPECT_TRUE(file.Hash(&hash, {"bycicle"_l}, &error)) << error;

  // Can't hash a directory.
  File dir(temp_dir);
  EXPECT_ANY_THROW(dir.Hash(&hash));
}

TEST(FileTest, Copy) {
  const auto expected_content1 = "All your base are belong to us"_l;
  const auto expected_content2 = "Nothing lasts forever"_l;
  const TemporaryDir temp_dir;
  const Path temp_dir_path = temp_dir.GetPath();
  const Path file1 = temp_dir_path / "1";
  const Path file2 = temp_dir_path / "2";
  const Path file3 = temp_dir_path / "3";

  ASSERT_TRUE(File::Write(file1, expected_content1));
  ASSERT_TRUE(File::Copy(file1, file2));

  const auto permissions = Perms::owner_read | Perms::owner_write |
                           Perms::group_read | Perms::others_read;

  std::error_code ec;

  Immutable content(true);
  // |content| is assignable since we use it many times during test.

  EXPECT_EQ(1u, std::experimental::filesystem::hard_link_count(file1, ec));
  ASSERT_FALSE(ec);

  const auto& file1_status = std::experimental::filesystem::status(file1, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(permissions, file1_status.permissions() & permissions);


  EXPECT_EQ(1u, std::experimental::filesystem::hard_link_count(file2, ec));
  ASSERT_FALSE(ec);

  auto file2_status = std::experimental::filesystem::status(file2, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(permissions, file2_status.permissions() & permissions);
  String error;
  ASSERT_TRUE(File::Read(file2, &content, &error)) << error;
  EXPECT_EQ(expected_content1, content);

  ASSERT_TRUE(File::Write(file3, expected_content2));
  ASSERT_TRUE(File::Copy(file3, file2));

  EXPECT_EQ(1u, std::experimental::filesystem::hard_link_count(file2, ec));
  ASSERT_FALSE(ec);

  file2_status = std::experimental::filesystem::status(file2, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(permissions, file2_status.permissions() & permissions);

  ASSERT_TRUE(File::Read(file2, &content, &error)) << error;
  EXPECT_EQ(expected_content2, content);
}

}  // namespace base
}  // namespace dist_clang
