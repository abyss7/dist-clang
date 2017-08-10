#include <base/process_impl.h>

#include <base/c_utils.h>
#include <base/string_utils.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {

namespace {

#if defined(OS_MACOSX)
// Bash is too old on a common MacOS and fails some tests
// (e.g. RunWithEnvironment), use zsh instead.
const char* sh = "/bin/zsh";
#else
const char* sh = "/bin/sh";
#endif

}  // namespace

namespace base {

class ProcessTest : public ::testing::Test {
 public:
  virtual void SetUp() override {
    base::Process::SetFactory<base::Process::DefaultFactory>();
  }
};

TEST_F(ProcessTest, CheckExitCode) {
  const int exit_code = 1;
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l)
      .AppendArg(Immutable("exit " + std::to_string(exit_code)));
  ASSERT_FALSE(process->Run(1));
}

TEST_F(ProcessTest, ChangeCurrentDir) {
  const String dir = "/usr";
  ASSERT_NE(dir, GetCurrentDir()) << "Don't run this test from " << dir;
  ProcessPtr process = Process::Create(sh, dir, Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("pwd"_l);
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(dir + "\n", process->stdout().string_copy());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadStderr) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l)
      .AppendArg(Immutable("echo " + test_data + " 1>&2"));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stderr());
  EXPECT_TRUE(process->stdout().empty());
}

TEST_F(ProcessTest, ReadSmallOutput) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(Immutable("echo " + test_data));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadLargeOutput) {
  const String test_data(67000, 'a');
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(Immutable("echo " + test_data));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, EchoSmallInput) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("cat"_l);
  ASSERT_TRUE(process->Run(1, Immutable(test_data)));
  EXPECT_EQ(Immutable(test_data), process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, EchoLargeInput) {
  const String test_data(67000, 'a');
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("cat"_l);
  ASSERT_TRUE(process->Run(1, Immutable(test_data)));
  EXPECT_EQ(Immutable(test_data), process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadTimeout) {
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("sleep 2"_l);
  ASSERT_FALSE(process->Run(1));
}

TEST_F(ProcessTest, TooManyArgs) {
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  for (auto i = 0u; i < ProcessImpl::MAX_ARGS + 2; ++i) {
    process->AppendArg("yes"_l);
  }
  ASSERT_THROW_STD(process->Run(1), "Assertion failed: .* < MAX_ARGS");
}

TEST_F(ProcessTest, RunWithEnvironment) {
  const auto expected_value = "some_value"_l;
  ProcessPtr process = Process::Create(sh, Path(), Process::SAME_UID);
  process->AddEnv("ENV", expected_value)
      .AppendArg("-c"_l)
      .AppendArg("echo -n $ENV"_l);
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(expected_value, process->stdout());
  EXPECT_TRUE(process->stderr().empty());

  // TODO: check that environment is preserved.
}

TEST_F(ProcessTest, DISABLED_WaitPidWithTimeOut) {
  // TODO: implement this test.
}

// FIXME: we shouldn't wait the signal for a second here
TEST_F(ProcessTest, ProcessAborts) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("kill -ABRT $$ && sleep 1"_l);
  ASSERT_FALSE(process->Run(1));
}

// FIXME: we shouldn't wait the signal for a second here
TEST_F(ProcessTest, ProcessCrashes) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("kill -SEGV $$ && sleep 1"_l);
  ASSERT_FALSE(process->Run(1));
}

}  // namespace base
}  // namespace dist_clang
