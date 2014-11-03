#pragma once

#include <base/aliases.h>

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

inline bool IsListening(FileDescriptor fd) {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

inline sigset_t BlockSignals() {
  sigset_t signal_set, old_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, SIGPROF);
  pthread_sigmask(SIG_SETMASK, &signal_set, &old_set);
  return old_set;
}

inline void UnblockSignals(sigset_t old_set) {
  pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
}

}  // namespace net
}  // namespace dist_clang
