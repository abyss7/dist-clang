#include "base/file_utils.h"

#include "base/temporary_dir.h"
#include "gtest/gtest.h"

#include <thread>

#include <fcntl.h>

namespace dist_clang {
namespace base {

TEST(FileUtilsTest, DISABLED_CopyFile) {
  // TODO: implement this.
  //  - Check hard-links.
  //  - Check raw copying.
  //  - Check file permissions.
  //  - Check contents.
}

TEST(FileUtilsTest, DISABLED_FileExists) {
  // TODO: implement this.
}

TEST(FileUtilsTest, ReadFile) {
  const std::string expected_content = "All your base are belong to us";

  base::TemporaryDir temp_dir;
  const std::string file_path = std::string(temp_dir) + "/file";
  int fd = open(file_path.c_str(), O_CREAT | O_WRONLY, 0777);
  ASSERT_NE(-1, fd);
  int size = write(fd, expected_content.data(), expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);

  std::string content;
  std::string error;
  EXPECT_TRUE(ReadFile(file_path, &content, &error)) << error;
  EXPECT_EQ(expected_content, content);
}

TEST(FileUtilsTest, WriteFile) {
  const std::string expected_content = "All your base are belong to us";

  base::TemporaryDir temp_dir;
  const std::string file_path = std::string(temp_dir) + "/file";
  EXPECT_TRUE(WriteFile(file_path, expected_content));

  char content[expected_content.size()];
  int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_NE(-1, fd);
  int size = read(fd, content, expected_content.size());
  ASSERT_EQ(expected_content.size(), static_cast<size_t>(size));
  close(fd);
  EXPECT_EQ(expected_content, std::string(content));
}

TEST(FileUtilsTest, CalculateDirectorySize) {
  base::TemporaryDir temp_dir;
  const std::string dir1 = std::string(temp_dir) + "/1";
  const std::string dir2 = std::string(temp_dir) + "/2";
  const std::string file1 = std::string(temp_dir) + "/file1";
  const std::string file2 = dir1 + "/file2";
  const std::string file3 = dir2 + "/file3";
  const std::string content1 = "a";
  const std::string content2 = "ab";
  const std::string content3 = "abc";

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

  std::string error;
  EXPECT_EQ(content1.size() + content2.size() + content3.size(),
            CalculateDirectorySize(temp_dir, &error))
      << error;
}

TEST(FileUtilsTest, FileSize) {
  base::TemporaryDir temp_dir;
  const std::string file = std::string(temp_dir) + "/file";
  const std::string content = "1234567890";

  int fd = open(file.c_str(), O_CREAT | O_WRONLY, 0777);
  ASSERT_NE(-1, fd);
  ASSERT_EQ(content.size(),
            static_cast<size_t>(write(fd, content.data(), content.size())));
  close(fd);

  EXPECT_EQ(content.size(), FileSize(file));
}

TEST(FileUtilsTest, LeastRecentPath) {
  base::TemporaryDir temp_dir;
  const std::string dir = std::string(temp_dir) + "/1";
  const std::string file1 = std::string(temp_dir) + "/2";
  const std::string file2 = dir + "/3";
  const std::string file3 = dir + "/4";

  ASSERT_NE(-1, mkdir(dir.c_str(), 0777));

  std::this_thread::sleep_for(std::chrono::seconds(1));
  int fd = open(file1.c_str(), O_CREAT, 0777);
  ASSERT_NE(-1, fd);
  close(fd);

  std::string path;
  EXPECT_TRUE(GetLeastRecentPath(temp_dir, path));
  EXPECT_EQ(dir, path) << "dir mtime is " << GetLastModificationTime(dir).first
                       << ":" << GetLastModificationTime(dir).second
                       << " ; path mtime is "
                       << GetLastModificationTime(path).first << ":"
                       << GetLastModificationTime(path).second;

  std::this_thread::sleep_for(std::chrono::seconds(1));
  fd = open(file2.c_str(), O_CREAT, 0777);
  ASSERT_NE(-1, fd);
  close(fd);

  EXPECT_TRUE(GetLeastRecentPath(temp_dir, path));
  EXPECT_EQ(file1, path) << "file1 mtime is "
                         << GetLastModificationTime(file1).first << ":"
                         << GetLastModificationTime(file1).second
                         << " ; path mtime is "
                         << GetLastModificationTime(path).first << ":"
                         << GetLastModificationTime(path).second;

  std::this_thread::sleep_for(std::chrono::seconds(1));
  fd = open(file3.c_str(), O_CREAT, 0777);
  ASSERT_NE(-1, fd);
  close(fd);

  EXPECT_TRUE(GetLeastRecentPath(dir, path));
  EXPECT_EQ(file2, path) << "file2 mtime is "
                         << GetLastModificationTime(file2).first << ":"
                         << GetLastModificationTime(file2).second
                         << " ; path mtime is "
                         << GetLastModificationTime(path).first << ":"
                         << GetLastModificationTime(path).second;
}

TEST(FileUtilsTest, TempFile) {
  std::string error;
  std::string temp_file = CreateTempFile(&error);

  ASSERT_FALSE(temp_file.empty()) << "Failed to create temporary file: "
                                  << error;
  ASSERT_TRUE(FileExists(temp_file));
  ASSERT_TRUE(DeleteFile(temp_file));
}

}  // namespace base
}  // namespace dist_clang
