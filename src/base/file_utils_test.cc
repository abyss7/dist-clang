#include "base/file_utils.h"

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(FileUtilsTest, DISABLED_CopyFile) {
  // TODO: implement this.
  //  - Check hard-links.
  //  - Check raw copying.
  //  - Check file permissions.
  //  - Check contents.
}

TEST(FileUtilsTest, ReadFile) {
  const char* expected_content = "All your base are belong to us";
  auto content_size = strlen(expected_content);
  char pattern[] = "/tmp/file-XXXXXX";
  int fd = mkstemp(pattern);
  ASSERT_NE(-1, fd);
  EXPECT_EQ(content_size,
            static_cast<size_t>(write(fd, expected_content, content_size)));
  close(fd);

  std::string content;
  EXPECT_TRUE(ReadFile(pattern, &content));
  EXPECT_EQ(expected_content, content);
  DeleteFile(pattern);
}

TEST(FileUtilsTest, WriteFile) {
  const char* expected_content = "All your base are belong to us";
  char pattern[] = "/tmp/file-XXXXXX";
  int fd = mkstemp(pattern);
  ASSERT_NE(-1, fd);
  close(fd);
  EXPECT_TRUE(WriteFile(pattern, expected_content));

  std::string content;
  EXPECT_TRUE(ReadFile(pattern, &content));
  EXPECT_EQ(expected_content, content);
  DeleteFile(pattern);
}

}  // namespace base
}  // namespace dist_clang
