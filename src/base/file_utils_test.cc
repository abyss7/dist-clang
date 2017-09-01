#include <base/file_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>
#include STL(thread)

#include <dirent.h>
#include <fcntl.h>

namespace dist_clang {
namespace base {

TEST(FileUtilsTest, CalculateDirectorySize) {
  const TemporaryDir temp_dir;
  const auto dir1 = temp_dir.path() / "1";
  const auto dir2 = temp_dir.path() / "2";
  const auto file1 = temp_dir.path() / "file1";
  const auto file2 = dir1 / "file2";
  const auto file3 = dir2 / "file3";
  const String content1 = "a";
  const String content2 = "ab";
  const String content3 = "abc";

  ASSERT_NE(-1, mkdir(dir1.c_str(), 0777));
  ASSERT_NE(-1, mkdir(dir2.c_str(), 0777));
  int fd1 = open(file1.c_str(), O_CREAT | O_WRONLY);
  int fd2 = open(file2.c_str(), O_CREAT | O_WRONLY);
  int fd3 = open(file3.c_str(), O_CREAT | O_WRONLY);
  ASSERT_TRUE(fd1 != -1 && fd2 != -1 && fd3 != -1);
  ASSERT_EQ(content1.size(),
            static_cast<size_t>(write(fd1, content1.data(), content1.size())));
  ASSERT_EQ(content2.size(),
            static_cast<size_t>(write(fd2, content2.data(), content2.size())));
  ASSERT_EQ(content3.size(),
            static_cast<size_t>(write(fd3, content3.data(), content3.size())));
  close(fd1);
  close(fd2);
  close(fd3);

  String error;
  EXPECT_EQ(content1.size() + content2.size() + content3.size(),
            CalculateDirectorySize(temp_dir, &error))
      << error;
}

TEST(FileUtilsTest, TempFile) {
  String error;
  const String temp_file = CreateTempFile(&error);

  ASSERT_FALSE(temp_file.empty())
      << "Failed to create temporary file: " << error;
  ASSERT_TRUE(File::Exists(temp_file));
  ASSERT_TRUE(File::Delete(temp_file));
}

TEST(FileUtilsTest, CreateDirectory) {
  String error;
  const TemporaryDir temp_dir;
  const auto temp = temp_dir.path() / "1" / "2" / "3";

  ASSERT_TRUE(CreateDirectory(temp, &error)) << error;

  DIR* dir = opendir(temp.c_str());
  EXPECT_TRUE(dir);
  if (dir) {
    closedir(dir);
  }
}

}  // namespace base
}  // namespace dist_clang
