#pragma once

#include <base/process.h>

namespace dist_clang {
namespace base {

class Pipe;

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
  // Kills the child with the parent - called in the child: only the
  // async-signal-safe functions.
  static void DieWithParent();
  bool WaitPid(int pid, ui64 sec_timeout, String* error = nullptr);
  void kill(int pid);

  bool killed_;
  // Signals |Cancel()| to the running process.
  int cancel_event_ = -1;
};

}  // namespace base
}  // namespace dist_clang
