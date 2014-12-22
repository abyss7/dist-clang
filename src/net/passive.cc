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
  auto fd = accept(native(), nullptr, nullptr);

  if (fd == -1) {
#if defined(OS_LINUX)
    // Linux accept() passes already-pending network errors on the new
    // socket as an error code from accept(). For reliable operation the
    // application should detect the network errors defined for the
    // protocol after accept() and treat them like EAGAIN by retrying.
    if (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
        errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH ||
        errno == EOPNOTSUPP || errno == ENETUNREACH) {
      errno = EAGAIN;
    }
#endif  // defined(OS_LINUX)
    DCHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    return Socket();
  }

  Socket socket(fd);
  socket.CloseOnExec();

  return std::move(socket);
}

}  // namespace net
}  // namespace dist_clang
