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
  auto fd = AcceptNative();

  if (fd == -1) {
    DCHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    return Socket();
  }

  Socket socket(fd);
  socket.CloseOnExec();
  socket.MakeBlocking(true);

  return socket;
}

}  // namespace net
}  // namespace dist_clang
