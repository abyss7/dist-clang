#pragma once

#include "net/base/types.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

inline bool MakeNonBlocking(fd_t fd, bool blocking = false) {
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

inline bool IsListening(fd_t fd) {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

}  // namespace net
}  // namespace dist_clang
