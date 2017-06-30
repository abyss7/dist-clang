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

 private:
  friend class DefaultFactory;

  explicit ProcessImpl(const Path& exec_path,
                       const Path& cwd_path = Path(), ui32 uid = SAME_UID);

  bool RunChild(Pipe& out, Pipe& err, Pipe* in);
  bool WaitPid(int pid, ui64 sec_timeout, String* error = nullptr);
  void kill(int pid);

  bool killed_;
};

}  // namespace base
}  // namespace dist_clang
