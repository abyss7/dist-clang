#include "net/kqueue_event_loop.h"

#include "base/assert.h"

#include <sys/event.h>

namespace dist_clang {
namespace net {

KqueueEventLoop::KqueueEventLoop(ConnectionCallback callback)
  : listen_fd_(kqueue()), io_fd_(kqueue()), callback_(callback) {
}

KqueueEventLoop::~KqueueEventLoop() {
  Stop();
  close(listen_fd_);
  close(io_fd_);
}

}  // namespace net
}  // namespace dist_clang
