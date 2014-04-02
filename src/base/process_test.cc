#include "base/process_impl.h"

#include "base/c_utils.h"
#include "base/string_utils.h"
#include "gtest/gtest.h"

namespace dist_clang {
namespace base {

TEST(ProcessTest, CheckExitCode) {
  const int exit_code = 1;
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("exit " + std::to_string(exit_code));
  ASSERT_FALSE(process->Run(1));
}

TEST(ProcessTest, ChangeCurrentDir) {
  const std::string dir = "/usr";
  ASSERT_NE(dir, GetCurrentDir()) << "Don't run this test from " + dir;
  ProcessPtr process = Process::Create("/bin/sh", dir, Process::SAME_UID);
  process->AppendArg("-c").AppendArg("pwd");
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(dir + "\n", process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST(ProcessTest, ReadStderr) {
  const std::string test_data(10, 'a');
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("echo " + test_data + " 1>&2");
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(test_data + "\n", process->stderr());
  EXPECT_TRUE(process->stdout().empty());
}

TEST(ProcessTest, ReadSmallOutput) {
  const std::string test_data(10, 'a');
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("echo " + test_data);
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(test_data + "\n", process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST(ProcessTest, ReadLargeOutput) {
  const std::string test_data(67000, 'a');
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("echo " + test_data);
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(test_data + "\n", process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST(ProcessTest, EchoSmallInput) {
  const std::string test_data(10, 'a');
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process->Run(1, test_data));
  EXPECT_EQ(test_data, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST(ProcessTest, EchoLargeInput) {
  const std::string test_data(67000, 'a');
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("cat");
  ASSERT_TRUE(process->Run(1, test_data));
  EXPECT_EQ(test_data, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST(ProcessTest, ReadTimeout) {
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  process->AppendArg("-c").AppendArg("sleep 2");
  ASSERT_FALSE(process->Run(1));
}

TEST(ProcessTest, TooManyArgs) {
  ProcessPtr process = Process::Create("/bin/sh", std::string(),
                                       Process::SAME_UID);
  for (int i = 0; i < ProcessImpl::MAX_ARGS + 2; ++i) {
    process->AppendArg("yes");
  }
  ASSERT_THROW_STD(process->Run(1), "Assertion failed: .* < MAX_ARGS");
}

}  // namespace base
}  // namespace dist_clang
