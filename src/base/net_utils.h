#pragma once

#include <fcntl.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

inline bool MakeNonBlocking(int fd, bool blocking = false) {
  int flags, s;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return false;

  if (!blocking)
    flags |= O_NONBLOCK;
  else
    flags ^= O_NONBLOCK;
  s = fcntl(fd, F_SETFL, flags);
  if (s == -1)
    return false;

  return true;
}

}  // namespace base
}  // namespace dist_clang
