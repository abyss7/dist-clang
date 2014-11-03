#pragma once

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
}  // namespace client

namespace daemon {
FORWARD_TEST(AbsorberTest, StoreLocalCache);
FORWARD_TEST(BaseDaemonTest, CreateProcessFromFlags);
FORWARD_TEST(EmitterTest, StoreCacheForLocalResult);
FORWARD_TEST(EmitterTest, StoreCacheForRemoteResult);
}  // namespace daemon

namespace base {

class ProcessImpl;

class Process : public Testable<Process, ProcessImpl, const String&,
                                const String&, ui32> {
 public:
  enum { UNLIMITED = 0, SAME_UID = 0 };

  explicit Process(const String& exec_path, const String& cwd_path = String(),
                   ui32 uid = SAME_UID);
  virtual ~Process() {}

  template <class Iterator>
  Process& AppendArg(Iterator begin, Iterator end) {
    args_.insert(args_.end(), begin, end);
    return *this;
  }
  Process& AppendArg(const String& arg);
  Process& AddEnv(const char* name, const String& value);

  inline const String& stdout() const { return stdout_; }
  inline const String& stderr() const { return stderr_; }

  // |sec_timeout| specifies the timeout in seconds - for how long we should
  // wait for another portion of the output from a child process.
  virtual bool Run(ui16 sec_timeout, String* error = nullptr) = 0;
  virtual bool Run(ui16 sec_timeout, const String& input,
                   String* error = nullptr) = 0;

 protected:
  const String exec_path_, cwd_path_;
  List<String> args_;
  List<String> envs_;
  String stdout_, stderr_;
  const ui32 uid_;

 private:
  FRIEND_TEST(client::ClientTest, NoInputFile);
  FRIEND_TEST(client::ClientTest, CannotSendMessage);
  FRIEND_TEST(client::ClientTest, CannotReadMessage);
  FRIEND_TEST(client::ClientTest, ReadMessageWithoutStatus);
  FRIEND_TEST(client::ClientTest, ReadMessageWithBadStatus);
  FRIEND_TEST(client::ClientTest, SuccessfulCompilation);
  FRIEND_TEST(client::ClientTest, FailedCompilation);
  FRIEND_TEST(daemon::AbsorberTest, StoreLocalCache);
  FRIEND_TEST(daemon::BaseDaemonTest, CreateProcessFromFlags);
  FRIEND_TEST(daemon::EmitterTest, StoreCacheForLocalResult);
  FRIEND_TEST(daemon::EmitterTest, StoreCacheForRemoteResult);
};

}  // namespace base
}  // namespace dist_clang
