#pragma once

#include <base/aliases.h>

#include <fcntl.h>

namespace dist_clang {
namespace base {

inline bool MakeCloseOnExec(FileDescriptor fd) {
  return fcntl(fd, F_SETFD, FD_CLOEXEC) != -1;
}

inline bool MakeNonBlocking(FileDescriptor fd, bool blocking = false) {
  int flags, s;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }

  if (!blocking) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  s = fcntl(fd, F_SETFL, flags);
  if (s == -1) {
    return false;
  }

  return true;
}

inline bool IsNonBlocking(FileDescriptor fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }

  return flags & O_NONBLOCK;
}

}  // namespace base
}  // namespace dist_clang
