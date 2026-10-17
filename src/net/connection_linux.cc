#include <net/connection.h>

#include <poll.h>

namespace dist_clang {
namespace net {

// static
bool Connection::IsSocketPeerGone(int fd) {
  struct pollfd poll_fd = {fd, POLLRDHUP, 0};
  return poll(&poll_fd, 1, 0) == 1 &&
         (poll_fd.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

}  // namespace net
}  // namespace dist_clang
