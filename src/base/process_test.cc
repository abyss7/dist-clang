#include "base/process.h"

#include "base/c_utils.h"
#include "base/string_utils.h"

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ProcessTest, CheckExitCode) {
  const int exit_code = 1;
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("exit " + IntToString(exit_code));
  ASSERT_FALSE(process.Run(1));
}

TEST(ProcessTest, ChangeCurrentDir) {
  const std::string dir = "/usr";
  ASSERT_NE(dir, GetCurrentDir()) << "Don't run this test from " + dir;
  Process process("/bin/sh", dir);
  process.AppendArg("-c").AppendArg("pwd");
  ASSERT_TRUE(process.Run(1));
  EXPECT_EQ(dir + "\n", process.stdout());
  EXPECT_TRUE(process.stderr().empty());
}

TEST(ProcessTest, ReadStderr) {
  const std::string test_data(10, 'a');
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("echo " + test_data + " 1>&2");
  ASSERT_TRUE(process.Run(1));
  EXPECT_EQ(test_data + "\n", process.stderr());
  EXPECT_TRUE(process.stdout().empty());
}

TEST(ProcessTest, ReadSmallOutput) {
  const std::string test_data(10, 'a');
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("echo " + test_data);
  ASSERT_TRUE(process.Run(1));
  EXPECT_EQ(test_data + "\n", process.stdout());
  EXPECT_TRUE(process.stderr().empty());
}

TEST(ProcessTest, ReadLargeOutput) {
  const std::string test_data(67000, 'a');
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("echo " + test_data);
  ASSERT_TRUE(process.Run(1));
  EXPECT_EQ(test_data + "\n", process.stdout());
  EXPECT_TRUE(process.stderr().empty());
}

TEST(ProcessTest, EchoSmallInput) {
  const std::string test_data(10, 'a');
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process.Run(1, test_data));
  EXPECT_EQ(test_data, process.stdout());
  EXPECT_TRUE(process.stderr().empty());
}

TEST(ProcessTest, EchoLargeInput) {
  const std::string test_data(67000, 'a');
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process.Run(1, test_data));
  EXPECT_EQ(test_data, process.stdout());
  EXPECT_TRUE(process.stderr().empty());
}

TEST(ProcessTest, ReadTimeout) {
  Process process("/bin/sh");
  process.AppendArg("-c").AppendArg("sleep 2");
  ASSERT_FALSE(process.Run(1));
}

TEST(ProcessTest, DISABLED_CreateWithFlags) {
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
