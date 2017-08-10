#include <base/file_utils.h>

#include <base/c_utils.h>
#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>
#include STL(fstream)
#include STL(system_error)
#include STL(thread)

#include STL_EXPERIMENTAL(filesystem)

namespace dist_clang {
namespace base {

TEST(FileUtilsTest, CalculateDirectorySize) {
  const base::TemporaryDir temp_dir;
  const Path dir1 = Path(temp_dir) / "1";
  const Path dir2 = Path(temp_dir) / "2";
  const Path file1 = Path(temp_dir) / "file1";
  const Path file2 = dir1 / "file2";
  const Path file3 = dir2 / "file3";
  const String content1 = "a";
  const String content2 = "ab";
  const String content3 = "abc";

  std::error_code ec;

  std::experimental::filesystem::create_directory(dir1, ec);
  ASSERT_FALSE(ec) << ec.message();
  std::experimental::filesystem::create_directory(dir2, ec);
  ASSERT_FALSE(ec) << ec.message();

  std::ofstream f1(file1);
  std::ofstream f2(file2);
  std::ofstream f3(file3);
  ASSERT_TRUE(f1.good() && f2.good() && f3.good());

  f1 << content1;
  f2 << content2;
  f3 << content3;

  f1.close();
  f2.close();
  f3.close();

  String error;
  EXPECT_EQ(content1.size() + content2.size() + content3.size(),
            CalculateDirectorySize(temp_dir, &error))
      << error;
}

TEST(FileUtilsTest, TempFile) {
  String error;
  const String temp_file = CreateTempFile(&error);

  ASSERT_FALSE(temp_file.empty()) << "Failed to create temporary file: "
                                  << error;
  ASSERT_TRUE(File::Exists(temp_file));
  ASSERT_TRUE(File::Delete(temp_file));
}

TEST(FileUtilsTest, CreateDirectory) {
  String error;
  const base::TemporaryDir temp_dir;
  const Path& temp = Path(temp_dir) / "1" / "2" / "3";

  ASSERT_TRUE(CreateDirectory(temp, &error)) << error;

  std::error_code ec;
  EXPECT_TRUE(std::experimental::filesystem::exists(temp, ec));
  ASSERT_FALSE(ec) << ec.message();
}

}  // namespace base
}  // namespace dist_clang
