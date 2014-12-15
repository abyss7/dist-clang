#include <net/passive.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <net/socket.h>

#include <sys/socket.h>

namespace dist_clang {
namespace net {

namespace {

base::Handle&& MoveSocket(Socket&& socket) {
  DCHECK(!socket.IsBlocking());
  return std::move(socket);
}

}  // namespace

Passive::Passive(Socket&& fd, ui32 backlog)
    : Handle(MoveSocket(std::move(fd))) {
  if (listen(native(), backlog) == -1) {
    base::GetLastError(&error_);
  }
}

Socket Passive::Accept() {
  auto fd = accept4(native(), nullptr, nullptr, SOCK_CLOEXEC);

  if (fd == -1) {
    // Linux accept4() passes already-pending network errors on the new
    // socket as an error code from accept4(). For reliable operation the
    // application should detect the network errors defined for the
    // protocol after accept4() and treat them like EAGAIN by retrying.
    if (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
        errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH ||
        errno == EOPNOTSUPP || errno == ENETUNREACH) {
      errno = EAGAIN;
    }
    DCHECK(errno == EAGAIN || errno == EWOULDBLOCK);
  }

  return std::move(Socket(fd));
}

}  // namespace net
}  // namespace dist_clang
