#pragma once

#include <base/const_string.h>
#include <base/process_forward.h>
#include <base/testable.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

namespace dist_clang {

namespace client {
FORWARD_TEST(ClientTest, NoInputFile);
FORWARD_TEST(ClientTest, CannotSendMessage);
FORWARD_TEST(ClientTest, CannotReadMessage);
FORWARD_TEST(ClientTest, ReadMessageWithoutStatus);
FORWARD_TEST(ClientTest, ReadMessageWithBadStatus);
FORWARD_TEST(ClientTest, SuccessfulCompilation);
FORWARD_TEST(ClientTest, FailedCompilation);
FORWARD_TEST(ClientTest, SendPluginPath);
}  // namespace client

namespace daemon {
FORWARD_TEST(AbsorberTest, SuccessfulCompilationWithRewriteIncludes);
FORWARD_TEST(AbsorberTest, StoreLocalCacheWithoutBlacklist);
FORWARD_TEST(AbsorberTest, DoNotStoreLocalCacheWhenDisabled);
FORWARD_TEST(AbsorberTest, StoreLocalCacheWithBlacklist);
FORWARD_TEST(AbsorberTest, StoreLocalCacheWithAndWithoutBlacklist);
FORWARD_TEST(CollectorTest, SimpleReport);
FORWARD_TEST(CompilationDaemonTest, CreateProcessFromFlags);
FORWARD_TEST(EmitterTest, ConfigurationUpdateFromCoordinator);
FORWARD_TEST(EmitterTest, TasksGetReshardedOnConfigurationUpdate);
FORWARD_TEST(EmitterTest, TasksGetReshardedOnFailedRemote);
FORWARD_TEST(EmitterTest, LocalMessageWithPluginPath);
FORWARD_TEST(EmitterTest, LocalMessageWithSanitizeBlacklist);
FORWARD_TEST(EmitterTest, ConfigurationWithoutVersions);
FORWARD_TEST(EmitterTest, LocalSuccessfulCompilation);
FORWARD_TEST(EmitterTest, StoreSimpleCacheForLocalResult);
FORWARD_TEST(EmitterTest, StoreSimpleCacheForRemoteResult);
FORWARD_TEST(EmitterTest,
             StoreDirectCacheForLocalResultWithAndWithoutIncludedHeaders);
FORWARD_TEST(EmitterTest, FallbackToLocalCompilationAfterRemoteFail);
FORWARD_TEST(EmitterTest, FallbackToLocalCompilationAfterRemoteRejects);
FORWARD_TEST(EmitterTest,
             StoreSimpleCacheForLocalResultWithAndWithoutBlacklist);
FORWARD_TEST(EmitterTest, StoreDirectCacheForLocalResult);
FORWARD_TEST(EmitterTest, StoreDirectCacheForRemoteResult);
FORWARD_TEST(EmitterTest, ConfigurationUpdateCompiler);
FORWARD_TEST(EmitterTest, HitDirectCacheFromTwoLocations);
FORWARD_TEST(EmitterTest, DontHitDirectCacheFromTwoRelativeSources);
}  // namespace daemon

namespace base {

class ProcessImpl;

class Process
    : public Testable<Process, ProcessImpl, const Path&, const Path&, ui32> {
 public:
  enum : ui16 { UNLIMITED = 0 };
  enum : ui32 { SAME_UID = 0 };

  explicit Process(const Path& exec_path, const Path& cwd_path = Path(),
                   ui32 uid = SAME_UID);
  virtual ~Process() {}

  template <class Iterator>
  Process& AppendArg(Iterator begin, Iterator end) {
    for (auto it = begin; it != end; ++it) {
      args_.push_back(Immutable(*it));
    }
    return *this;
  }
  Process& AppendArg(Immutable arg);
  Process& AddEnv(const char* name, const char* value);

#if defined(OS_WIN)
// FIXME: cursed Windows STL defines these as macros.
#undef stdout
#undef stderr
#endif  // defined(OS_WIN)
  inline Immutable stdout() const { return stdout_; }
  inline Immutable stderr() const { return stderr_; }

  // |sec_timeout| specifies the timeout in seconds - for how long we should
  // wait for another portion of the output from a child process.
  virtual bool Run(ui16 sec_timeout, String* error = nullptr) = 0;
  virtual bool Run(ui16 sec_timeout, Immutable input,
                   String* error = nullptr) = 0;

 protected:
  const Path exec_path_, cwd_path_;
  List<Immutable> args_, envs_;
  Immutable stdout_, stderr_;
  const ui32 uid_;

 private:
  FRIEND_TEST(client::ClientTest, NoInputFile);
  FRIEND_TEST(client::ClientTest, CannotSendMessage);
  FRIEND_TEST(client::ClientTest, CannotReadMessage);
  FRIEND_TEST(client::ClientTest, ReadMessageWithoutStatus);
  FRIEND_TEST(client::ClientTest, ReadMessageWithBadStatus);
  FRIEND_TEST(client::ClientTest, SuccessfulCompilation);
  FRIEND_TEST(client::ClientTest, FailedCompilation);
  FRIEND_TEST(client::ClientTest, SendPluginPath);
  FRIEND_TEST(daemon::AbsorberTest, SuccessfulCompilationWithRewriteIncludes);
  FRIEND_TEST(daemon::AbsorberTest, StoreLocalCacheWithoutBlacklist);
  FRIEND_TEST(daemon::AbsorberTest, DoNotStoreLocalCacheWhenDisabled);
  FRIEND_TEST(daemon::AbsorberTest, StoreLocalCacheWithBlacklist);
  FRIEND_TEST(daemon::AbsorberTest, StoreLocalCacheWithAndWithoutBlacklist);
  FRIEND_TEST(daemon::CollectorTest, SimpleReport);
  FRIEND_TEST(daemon::CompilationDaemonTest, CreateProcessFromFlags);
  FRIEND_TEST(daemon::EmitterTest, ConfigurationUpdateFromCoordinator);
  FRIEND_TEST(daemon::EmitterTest, TasksGetReshardedOnConfigurationUpdate);
  FRIEND_TEST(daemon::EmitterTest, TasksGetReshardedOnFailedRemote);
  FRIEND_TEST(daemon::EmitterTest, LocalMessageWithPluginPath);
  FRIEND_TEST(daemon::EmitterTest, LocalMessageWithSanitizeBlacklist);
  FRIEND_TEST(daemon::EmitterTest, ConfigurationWithoutVersions);
  FRIEND_TEST(daemon::EmitterTest, LocalSuccessfulCompilation);
  FRIEND_TEST(daemon::EmitterTest, StoreSimpleCacheForLocalResult);
  FRIEND_TEST(daemon::EmitterTest, StoreSimpleCacheForRemoteResult);
  FRIEND_TEST(daemon::EmitterTest, FallbackToLocalCompilationAfterRemoteFail);
  FRIEND_TEST(daemon::EmitterTest,
              FallbackToLocalCompilationAfterRemoteRejects);
  FRIEND_TEST(daemon::EmitterTest,
              StoreDirectCacheForLocalResultWithAndWithoutIncludedHeaders);
  FRIEND_TEST(daemon::EmitterTest,
              StoreSimpleCacheForLocalResultWithAndWithoutBlacklist);
  FRIEND_TEST(daemon::EmitterTest, StoreDirectCacheForLocalResult);
  FRIEND_TEST(daemon::EmitterTest, StoreDirectCacheForRemoteResult);
  FRIEND_TEST(daemon::EmitterTest, ConfigurationUpdateCompiler);
  FRIEND_TEST(daemon::EmitterTest, HitDirectCacheFromTwoLocations);
  FRIEND_TEST(daemon::EmitterTest, DontHitDirectCacheFromTwoRelativeSources);
};

}  // namespace base
}  // namespace dist_clang
