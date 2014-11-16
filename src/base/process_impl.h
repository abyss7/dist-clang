#pragma once

#include <base/process.h>

namespace dist_clang {
namespace base {

class ProcessImpl : public Process {
 public:
  enum : ui32 { MAX_ARGS = 4096 };

  virtual bool Run(ui16 sec_timeout, String* error = nullptr) override;
  virtual bool Run(ui16 sec_timeout, Immutable input,
                   String* error = nullptr) override;

 private:
  friend class DefaultFactory;

  class ScopedDescriptor {
   public:
    ScopedDescriptor(FileDescriptor fd);
    ~ScopedDescriptor();

    operator FileDescriptor();
    FileDescriptor Release();

   private:
    FileDescriptor fd_;
  };

  explicit ProcessImpl(const String& exec_path,
                       Immutable cwd_path = Immutable(), ui32 uid = SAME_UID);

  bool RunChild(FileDescriptor(&out_pipe)[2], FileDescriptor(&err_pipe)[2],
                FileDescriptor* in_pipe);
  void kill(int pid);

  bool killed_;
};

}  // namespace base
}  // namespace dist_clang
