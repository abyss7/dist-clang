#pragma once

#include <base/aliases.h>

#include <sys/socket.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

inline bool IsListening(FileDescriptor fd) {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

}  // namespace net
}  // namespace dist_clang
