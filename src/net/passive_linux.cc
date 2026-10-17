#include <net/passive.h>

#include <errno.h>
#include <sys/socket.h>

namespace dist_clang {
namespace net {

int Passive::AcceptNative() {
  const int fd = accept4(native(), nullptr, nullptr, SOCK_CLOEXEC);
  // Linux accept() passes already-pending network errors on the new socket as
  // an error code from accept(). For reliable operation the application should
  // detect the network errors defined for the protocol after accept() and
  // treat them like EAGAIN by retrying.
  if (fd == -1 &&
      (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
       errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH ||
       errno == EOPNOTSUPP || errno == ENETUNREACH)) {
    errno = EAGAIN;
  }
  return fd;
}

}  // namespace net
}  // namespace dist_clang
