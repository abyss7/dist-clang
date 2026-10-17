#include <net/connection.h>

#include <poll.h>
#include <sys/socket.h>

namespace dist_clang {
namespace net {

// static
bool Connection::IsSocketPeerGone(int fd) {
  // There is no POLLRDHUP: the end of the stream is readable without data.
  struct pollfd poll_fd = {fd, POLLIN, 0};
  if (poll(&poll_fd, 1, 0) != 1) {
    return false;
  }
  if (poll_fd.revents & (POLLHUP | POLLERR)) {
    return true;
  }
  char byte;
  return recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

}  // namespace net
}  // namespace dist_clang
