#pragma once

#include "base/process.h"
#include "net/base/types.h"

namespace dist_clang {
namespace base {

class ProcessImpl : public Process {
 public:
  enum {
    MAX_ARGS = 4096
  };

  virtual bool Run(ui16 sec_timeout, std::string* error = nullptr) override;
  virtual bool Run(ui16 sec_timeout, const std::string& input,
                   std::string* error = nullptr) override;

 private:
  friend class DefaultFactory;

  class ScopedDescriptor {
   public:
    ScopedDescriptor(net::fd_t fd);
    ~ScopedDescriptor();

    operator net::fd_t();
    net::fd_t Release();

   private:
    net::fd_t fd_;
  };

  explicit ProcessImpl(const std::string& exec_path,
                       const std::string& cwd_path = std::string(),
                       ui32 uid = SAME_UID);

  bool RunChild(int (&out_pipe)[2], int (&err_pipe)[2], int* in_pipe);
  void kill(int pid);

  bool killed_;
};

}  // namespace base
}  // namespace dist_clang
