#pragma once

#include "net/base/types.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

inline bool MakeNonBlocking(fd_t fd, bool blocking = false) {
  int flags, s;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }

  if (!blocking) {
    flags |= O_NONBLOCK;
  }
  else {
    flags &= ~O_NONBLOCK;
  }
  s = fcntl(fd, F_SETFL, flags);
  if (s == -1) {
    return false;
  }

  return true;
}

inline bool IsNonBlocking(fd_t fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return false;

  return flags & O_NONBLOCK;
}

inline bool IsListening(fd_t fd) {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

inline sigset_t BlockSignals() {
  sigset_t signal_set, old_set;

  sigfillset(&signal_set);
  pthread_sigmask(SIG_SETMASK, &signal_set, &old_set);
  return old_set;
}

inline void UnblockSignals(sigset_t old_set) {
  pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
}

}  // namespace net
}  // namespace dist_clang
