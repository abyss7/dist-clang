#include <base/process_impl.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/platform.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>

#include <test/throw_std.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include STL(thread)

namespace dist_clang {

namespace base {

// The shell of the tests - differs between the platforms: in
// "process_test_shell_<os>.cc".
extern const char* const sh;

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

TEST_F(ProcessTest, ExitedByItself) {
  // The failure of the process itself.
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("exit 1"_l);
  ASSERT_FALSE(process->Run(1));
  EXPECT_TRUE(process->exited());

  // The process is killed.
  process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("kill -9 $$"_l);
  ASSERT_FALSE(process->Run(1));
  EXPECT_FALSE(process->exited());

  // The process doesn't run at all.
  process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("exit 0"_l);
  process->Cancel();
  ASSERT_FALSE(process->Run(1));
  EXPECT_FALSE(process->exited());
}

TEST_F(ProcessTest, WatchedSocketNotInherited) {
  // The descriptors of a process - but of "ls" itself.
  auto CountDescriptors = [] {
    ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
    process->AppendArg("-c"_l).AppendArg("ls /dev/fd/ | wc -l"_l);
    EXPECT_TRUE(process->Run(10));
    return process->stdout().string_copy();
  };
  const String alone = CountDescriptors();

  int sockets[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
  fcntl(sockets[1], F_SETFD, FD_CLOEXEC);

  // The process, that watches the socket of its client, runs.
  std::thread watching([&] {
    ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
    process->AppendArg("-c"_l).AppendArg("sleep 1"_l);
    process->CancelOnDisconnect(sockets[0]);
    EXPECT_TRUE(process->Run(10));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // The other process, that starts meanwhile, doesn't get the socket.
  EXPECT_EQ(alone, CountDescriptors());

  watching.join();
  close(sockets[0]);
  close(sockets[1]);
}

TEST_F(ProcessTest, DieWithGoneParent) {
  if (CurrentPlatform() != Platform::LINUX) {
    GTEST_SKIP() << "The child outlives the parent";
  }

  // The parent, that has started the child, is gone before the check: the
  // child has another parent then.
  pid_t child = fork();
  if (child == 0) {
    ProcessImpl::DieWithParent(1);
    _exit(0);
  }
  int status;
  ASSERT_EQ(child, waitpid(child, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(1, WEXITSTATUS(status));

  const pid_t parent = getpid();
  child = fork();
  if (child == 0) {
    ProcessImpl::DieWithParent(parent);
    _exit(0);
  }
  ASSERT_EQ(child, waitpid(child, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
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
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  // The child mustn't keep the connection.
  fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
  fcntl(sockets[1], F_SETFD, FD_CLOEXEC);

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

TEST_F(ProcessTest, CancelFromAnotherThread) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("sleep 30; echo done"_l);

  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    process->Cancel();
  });

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(process->Run(Process::UNLIMITED));
  cancel.join();

  EXPECT_TRUE(process->canceled());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
  EXPECT_TRUE(process->stdout().empty());
}

TEST_F(ProcessTest, CancelLetsCleanUp) {
  const TemporaryDir temp_dir;
  const Path temporary = temp_dir.path() / "output.tmp";

  // E.g. the compiler removes its temporary outputs on the termination.
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l)
      .AppendArg(Immutable("trap 'rm -f " + temporary.string() +
                           "; exit 1' TERM; touch " + temporary.string() +
                           "; sleep 30 & wait"));

  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    process->Cancel();
  });
  EXPECT_FALSE(process->Run(Process::UNLIMITED));
  cancel.join();

  EXPECT_TRUE(process->canceled());
  EXPECT_FALSE(base::File::Exists(temporary));
}

TEST_F(ProcessTest, CancelKillsProcessIgnoringTermination) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("trap '' TERM; sleep 30"_l);

  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    process->Cancel();
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(process->Run(Process::UNLIMITED));
  cancel.join();

  EXPECT_TRUE(process->canceled());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST_F(ProcessTest, CancelBeforeRun) {
  ProcessPtr process = Process::Create(sh, String(), Process::SAME_UID);
  process->AppendArg("-c"_l).AppendArg("echo done"_l);
  process->Cancel();

  EXPECT_FALSE(process->Run(Process::UNLIMITED));
  EXPECT_TRUE(process->canceled());
  EXPECT_TRUE(process->stdout().empty());
}

TEST_F(ProcessTest, NotCanceledWhileConnected) {
  int sockets[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  // The child mustn't keep the connection.
  fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
  fcntl(sockets[1], F_SETFD, FD_CLOEXEC);

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
