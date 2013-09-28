#include "base/process.h"

#include <gtest/gtest.h>

namespace dist_clang {
namespace testing {

TEST(ProcessTest, DISABLED_CheckExitCode) {
  // TODO: implement this.
}

TEST(ProcessTest, DISABLED_ChangeCurrentDir) {
  // TODO: implement this.
}

TEST(ProcessTest, DISABLED_ReadSmallOutput) {
  // TODO: implement this.
}

TEST(ProcessTest, ReadLargeOutput) {
  std::string test_data(4000, 'a');
  base::Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("echo " + test_data);
  ASSERT_TRUE(process.Run(1));
  EXPECT_EQ(test_data +"\n", process.stdout());
}

TEST(ProcessTest, EchoSmallInput) {
  std::string test_data(10, 'a');
  base::Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process.Run(1, test_data));
  EXPECT_EQ(test_data, process.stdout());
}

TEST(ProcessTest, EchoLargeInput) {
  std::string test_data(4000, 'a');
  base::Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process.Run(1, test_data));
  EXPECT_EQ(test_data, process.stdout());
}

}  // namespace testing
}  // namespace dist_clang
