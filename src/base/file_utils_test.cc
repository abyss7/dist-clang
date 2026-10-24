#include <base/file_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>
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

TEST(FileUtilsTest, WalkDirectoryWithVanishedFiles) {
  // The files vanish during the walk - like the temporary files of the cache:
  // the rest of the directory is walked anyway.
  const TemporaryDir temp_dir;
  const int count = 20;
  auto Name = [](const char* prefix, int i) {
    return prefix + std::to_string(i);
  };
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(File::Write(temp_dir.path() / Name("file", i), "a"_l));
    const auto dir = temp_dir.path() / Name("dir", i);
    ASSERT_NE(-1, mkdir(dir.c_str(), 0777));
    ASSERT_TRUE(File::Write(dir / "nested", "b"_l));
  }
  // Not followed.
  ASSERT_EQ(0, symlink("missing", (temp_dir.path() / "link").c_str()));

  ui32 files = 0, nested = 0;
  String error;
  WalkDirectory(
      temp_dir,
      [&](const Path& path, ui64, ui64) {
        if (path.filename() == "nested") {
          ++nested;
          return;
        }
        if (files++ == 0) {
          for (int i = 0; i < count; ++i) {
            const auto other = temp_dir.path() / Name("file", i);
            if (other != path) {
              File::Delete(other);
            }
          }
        }
      },
      &error);
  EXPECT_EQ(1u, files);
  EXPECT_EQ(ui32(count), nested);
  EXPECT_TRUE(error.empty()) << error;
}

TEST(FileUtilsTest, FileVersion) {
  const TemporaryDir temp_dir;
  const String path = temp_dir.path() / "dist-clangd";
  EXPECT_TRUE(FileVersion(path).empty());

  ASSERT_TRUE(File::Write(path, "old"_l));
  const String old_version = FileVersion(path);
  EXPECT_FALSE(old_version.empty());
  EXPECT_EQ(old_version, FileVersion(path));

  // Only the status is changed - e.g. "perf" links the executable into its
  // cache: the contents are the same.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const String link_path = temp_dir.path() / "link";
  ASSERT_EQ(0, link(path.c_str(), link_path.c_str()));
  ASSERT_EQ(0, chmod(path.c_str(), 0700));
  EXPECT_EQ(old_version, FileVersion(path));

  // The new version is installed - with the same size.
  ASSERT_TRUE(File::Write(path, "new"_l));
  EXPECT_NE(old_version, FileVersion(path));
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
  ASSERT_TRUE(CreateDirectory(temp, &error)) << error;

  DIR* dir = opendir(temp.c_str());
  EXPECT_TRUE(dir);
  if (dir) {
    closedir(dir);
  }
}

}  // namespace base
}  // namespace dist_clang
