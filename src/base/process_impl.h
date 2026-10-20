#pragma once

#include <base/process.h>

#include <unistd.h>

namespace dist_clang {
namespace base {

class Pipe;
FORWARD_TEST(ProcessTest, DieWithGoneParent);

class ProcessImpl : public Process {
 public:
  enum : ui32 { MAX_ARGS = 4096 };

  bool Run(ui16 sec_timeout, String* error = nullptr) override;
  bool Run(ui16 sec_timeout, Immutable input, String* error = nullptr) override;
  void Cancel() override;
  ~ProcessImpl() override;

 private:
  friend class DefaultFactory;

  explicit ProcessImpl(const Path& exec_path, const Path& cwd_path = Path(),
                       ui32 uid = SAME_UID);

  bool RunChild(Pipe& out, Pipe& err, Pipe* in);

  // The parts, that differ between the platforms - in "process_impl_<os>.cc".
  // The event, that |Cancel()| signals to the running process - or -1, if the
  // platform can't.
  static int CreateCancelEvent();
  static void SignalCancelEvent(int event);
  // Kills the child with the |parent|, that has started it - called in the
  // child: only the async-signal-safe functions.
  static void DieWithParent(pid_t parent);
  bool WaitPid(int pid, ui64 sec_timeout, String* error = nullptr);
  void kill(int pid);
  // Terminates the whole group of the canceled process: it may remove its
  // temporary files - and is killed, if it doesn't exit soon.
  void Terminate(int pid);

  FRIEND_TEST(ProcessTest, DieWithGoneParent);

  // The child may start, when this process is gone already: it isn't its
  // parent then.
  const pid_t parent_pid_ = getpid();
  bool killed_;
  bool terminated_ = false;
  // Signals |Cancel()| to the running process.
  int cancel_event_ = -1;
};

}  // namespace base
}  // namespace dist_clang
