#include <base/file_holder.h>

#include <base/file_utils.h>

#include <third_party/gtest/public/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(FileHolderTest, ReadFileAfterDelete) {
  const String tmp = base::CreateTempFile();
  const String expected_content = "some content";

  ASSERT_FALSE(tmp.empty());
  ASSERT_TRUE(base::WriteFile(tmp, expected_content));

  const FileHolder holder(tmp);
  ASSERT_TRUE(holder);

  EXPECT_TRUE(base::DeleteFile(tmp));

  String output;
  ASSERT_TRUE(base::ReadFile(holder, &output));
  EXPECT_EQ(expected_content, output);
}

}  // namespace base
}  // namespace dist_clang
