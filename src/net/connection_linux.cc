#include <net/connection.h>

#include <poll.h>
#include <sys/socket.h>

namespace dist_clang {
namespace net {

// static
bool Connection::IsSocketPeerGone(int fd) {
  struct pollfd poll_fd = {fd, POLLRDHUP, 0};
  return poll(&poll_fd, 1, 0) == 1 &&
         (poll_fd.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

// static
bool Connection::SocketPeerUid(int fd, ui32* uid) {
  struct ucred credentials;
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
    return false;
  }
  *uid = credentials.uid;
  return true;
}

}  // namespace net
}  // namespace dist_clang
