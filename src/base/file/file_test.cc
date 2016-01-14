#include <base/file/file.h>

#include <base/const_string.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

#include <fcntl.h>

namespace dist_clang {
namespace base {

TEST(FileTest, Read) {
  const auto expected_content = "All your base are belong to us"_l;
  const base::TemporaryDir temp_dir;
  const String file_path = String(temp_dir) + "/file";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, expected_content, expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);

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
  const String file_path = String(temp_dir) + "/file";

  String error;
  EXPECT_TRUE(File::Write(file_path, expected_content, &error)) << error;

  char content[expected_content.size()];
  int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_NE(-1, fd);
  int size = read(fd, content, expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);

  EXPECT_EQ(expected_content, String(content, expected_content.size()));

  // Can't write to directory.
  EXPECT_FALSE(File::Write(temp_dir, expected_content));
}

TEST(FileTest, Size) {
  const base::TemporaryDir temp_dir;
  const String file_path = String(temp_dir) + "/file";
  const String content = "1234567890";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, content.data(), content.size());
  ASSERT_EQ(content.size(), static_cast<size_t>(size));
  close(fd);

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
  const String file_path = String(temp_dir) + "/file";

  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, content, content.size());
  ASSERT_EQ(content.size(), static_cast<size_t>(size));
  close(fd);

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
  const String file1 = String(temp_dir) + "/1";
  const String file2 = String(temp_dir) + "/2";
  const String file3 = String(temp_dir) + "/3";

  ASSERT_TRUE(File::Write(file1, expected_content1));
  ASSERT_TRUE(File::Copy(file1, file2));

  struct stat st;
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  Immutable content(true);
  // |content| is assignable since we use it many times during test.

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  String error;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  ASSERT_TRUE(File::Read(file2, &content, &error)) << error;
  EXPECT_EQ(expected_content1, content);

  ASSERT_TRUE(File::Write(file3, expected_content2));
  ASSERT_TRUE(File::Copy(file3, file2));

  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  ASSERT_TRUE(File::Read(file2, &content));
  EXPECT_EQ(expected_content2, content);
}

TEST(FileTest, Link) {
  const auto expected_content = "All your base are belong to us"_l;
  const TemporaryDir temp_dir;
  const String file1 = String(temp_dir) + "/1";
  const String file2 = String(temp_dir) + "/2";
  const String file3 = String(temp_dir) + "/3";

  ASSERT_TRUE(File::Write(file1, expected_content));
  ASSERT_TRUE(File::Link(file1, file2));

  struct stat st;
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  auto inode = st.st_ino;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_EQ(inode, st.st_ino);

  ASSERT_TRUE(File::Write(file3, expected_content));
  ASSERT_TRUE(File::Link(file3, file2));

  ASSERT_EQ(0, stat(file1.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);

  inode = st.st_ino;
  ASSERT_EQ(0, stat(file2.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_NE(inode, st.st_ino);

  inode = st.st_ino;
  ASSERT_EQ(0, stat(file3.c_str(), &st));
  EXPECT_EQ(2u, st.st_nlink);
  EXPECT_EQ(mode, st.st_mode & mode);
  EXPECT_EQ(inode, st.st_ino);
}

}  // namespace base
}  // namespace dist_clang
