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
  base::Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("echo " + std::string(4000, 'a'));
  ASSERT_TRUE(process.Run(1, nullptr));
  EXPECT_EQ(std::string(4000, 'a') +"\n", process.stdout());
}

}  // namespace testing
}  // namespace dist_clang
