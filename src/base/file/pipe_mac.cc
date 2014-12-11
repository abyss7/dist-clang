#include <base/file/pipe.h>

#include <base/c_utils.h>

namespace dist_clang {
namespace base {

Pipe::Pipe(bool blocking) {
  int fds[2];

  if (pipe(fds) == -1) {
    GetLastError(&error_);
    return;
  }

  fds_[0] = Data(fds[0]);
  fds_[1] = Data(fds[1]);

  fds_[0].CloseOnExec();
  fds_[1].CloseOnExec();
  fds_[0].MakeBlocking(blocking);
  fds_[1].MakeBlocking(blocking);
}

}  // namespace base
}  // namespace dist_clang
