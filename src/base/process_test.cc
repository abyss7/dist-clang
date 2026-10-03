#include <base/process_impl.h>

#include <base/file_utils.h>
#include <base/string_utils.h>

#include <test/throw_std.h>

#include <sys/socket.h>
#include <unistd.h>

#include STL(thread)

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
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(
      Immutable("exit " + std::to_string(exit_code)));
  ASSERT_FALSE(process->Run(1));
}

TEST_F(ProcessTest, ChangeCurrentDir) {
  const auto dir = Path("/usr");
  ASSERT_NE(dir, GetCurrentDir()) << "Don't run this test from " << dir;
  ProcessPtr process = Process::Create(sh, dir, Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("pwd"_l);
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(dir) + "\n"_l, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadStderr) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(
      Immutable("echo " + test_data + " 1>&2"));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stderr());
  EXPECT_TRUE(process->stdout().empty());
}

TEST_F(ProcessTest, ReadSmallOutput) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(Immutable("echo " + test_data));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadLargeOutput) {
  const String test_data(67000, 'a');
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg(Immutable("echo " + test_data));
  ASSERT_TRUE(process->Run(1));
  EXPECT_EQ(Immutable(test_data) + "\n"_l, process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, EchoSmallInput) {
  const String test_data(10, 'a');
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("cat"_l);
  ASSERT_TRUE(process->Run(1, Immutable(test_data)));
  EXPECT_EQ(Immutable(test_data), process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, EchoLargeInput) {
  const String test_data(67000, 'a');
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("cat"_l);
  ASSERT_TRUE(process->Run(1, Immutable(test_data)));
  EXPECT_EQ(Immutable(test_data), process->stdout());
  EXPECT_TRUE(process->stderr().empty());
}

TEST_F(ProcessTest, ReadTimeout) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("sleep 2"_l);
  ASSERT_FALSE(process->Run(1));
}

TEST_F(ProcessTest, TooManyArgs) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  for (auto i = 0u; i < ProcessImpl::MAX_ARGS + 2; ++i) {
    process->AppendArg("yes"_l);
  }
  ASSERT_THROW_STD(process->Run(1), "Assertion failed: .* < MAX_ARGS");
}

TEST_F(ProcessTest, CancelOnDisconnect) {
  int sockets[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));

  // The child of the shell is killed too.
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("sleep 30; echo done"_l);
  process->CancelOnDisconnect(sockets[0]);

  std::thread disconnect([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    close(sockets[1]);
  });

  const auto start = std::chrono::steady_clock::now();
  String error;
  EXPECT_FALSE(process->Run(Process::UNLIMITED, &error));
  disconnect.join();

  EXPECT_TRUE(process->canceled());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
  EXPECT_TRUE(process->stdout().empty());
  close(sockets[0]);
}

TEST_F(ProcessTest, NotCanceledWhileConnected) {
  int sockets[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));

  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("echo done"_l);
  process->CancelOnDisconnect(sockets[0]);

  EXPECT_TRUE(process->Run(Process::UNLIMITED));
  EXPECT_FALSE(process->canceled());
  EXPECT_EQ("done\n"_l, process->stdout());

  close(sockets[0]);
  close(sockets[1]);
}

TEST_F(ProcessTest, RunWithEnvironment) {
  const auto expected_value = "some_value"_l;
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
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
