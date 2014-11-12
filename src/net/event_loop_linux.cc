#include <net/event_loop_linux.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file_descriptor_utils.h>
#include <net/connection_impl.h>
#include <net/net_utils.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

EpollEventLoop::EpollEventLoop(ConnectionCallback callback)
    : listen_fd_(epoll_create1(EPOLL_CLOEXEC)),
      io_fd_(epoll_create1(EPOLL_CLOEXEC)),
      callback_(callback) {
}

EpollEventLoop::~EpollEventLoop() {
  Stop();
  close(listen_fd_);
  close(io_fd_);
}

bool EpollEventLoop::HandlePassive(FileDescriptor fd) {
  DCHECK(IsListening(fd));
  DCHECK(base::IsNonBlocking(fd));
  listening_fds_.insert(fd);
  return ReadyForListen(fd);
}

bool EpollEventLoop::ReadyForRead(ConnectionImplPtr connection) {
  return ReadyFor(connection, EPOLLIN);
}

bool EpollEventLoop::ReadyForSend(ConnectionImplPtr connection) {
  return ReadyFor(connection, EPOLLOUT);
}

void EpollEventLoop::DoListenWork(const std::atomic<bool>& is_shutting_down,
                                  FileDescriptor self_pipe) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];

  {
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = self_pipe;
    epoll_ctl(listen_fd_, EPOLL_CTL_ADD, self_pipe, &event);
  }

  while (!is_shutting_down) {
    auto events_count = epoll_wait(listen_fd_, events, MAX_EVENTS, -1);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      if (events[i].data.fd == self_pipe) {
        continue;
      }

      DCHECK(events[i].events & EPOLLIN);
      auto fd = events[i].data.fd;
      while (true) {
        auto new_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (new_fd == -1) {
          // Linux accept4() passes already-pending network errors on the new
          // socket as an error code from accept(). For  reliable  operation
          // the application should detect the network errors defined for the
          // protocol after accept() and treat them like EAGAIN by retrying.
          if (errno == ENETDOWN || errno == EPROTO || errno == ENOPROTOOPT ||
              errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH ||
              errno == EOPNOTSUPP || errno == ENETUNREACH) {
            errno = EAGAIN;
          }
          DCHECK(errno == EAGAIN || errno == EWOULDBLOCK);
          break;
        }
        callback_(fd, ConnectionImpl::Create(*this, new_fd));
      }
      ReadyForListen(fd);
    }
  }

  for (auto fd : listening_fds_) {
    close(fd);
  }
}

void EpollEventLoop::DoIOWork(const std::atomic<bool>& is_shutting_down,
                              FileDescriptor self_pipe) {
  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = self_pipe;
  epoll_ctl(io_fd_, EPOLL_CTL_ADD, self_pipe, &event);

  while (!is_shutting_down) {
    auto events_count = epoll_wait(io_fd_, &event, 1, -1);
    if (events_count == -1) {
      if (errno != EINTR) {
        break;
      } else {
        continue;
      }
    }

    DCHECK(events_count == 1);
    FileDescriptor fd = event.data.fd;

    // FIXME: it's a little bit hacky, but should work almost always.
    if (fd == self_pipe) {
      continue;
    }

    auto raw_connection =
        reinterpret_cast<Connection*>(event.data.ptr)->shared_from_this();
    auto connection = std::static_pointer_cast<ConnectionImpl>(raw_connection);
    fd = GetConnectionDescriptor(connection);

    int data = 0;
    if (event.events & EPOLLERR || ioctl(fd, FIONREAD, &data) == -1 ||
        (event.events & EPOLLHUP && data == 0)) {
      ConnectionClose(connection);
    } else if (event.events & EPOLLIN) {
      ConnectionDoRead(connection);
    } else if (event.events & EPOLLOUT) {
      ConnectionDoSend(connection);
    } else {
      NOTREACHED();
    }
  }
}

bool EpollEventLoop::ReadyForListen(FileDescriptor fd) {
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = fd;
  if (epoll_ctl(listen_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno == ENOENT) {
      return epoll_ctl(listen_fd_, EPOLL_CTL_ADD, fd, &event) != -1;
    }
    return false;
  }
  return true;
}

bool EpollEventLoop::ReadyFor(ConnectionImplPtr connection, ui32 events) {
  DCHECK(connection->IsOnEventLoop(this));

  auto fd = GetConnectionDescriptor(connection);
  struct epoll_event event;
  event.events = events | EPOLLONESHOT;
  event.data.ptr = connection.get();
  return epoll_ctl(io_fd_, EPOLL_CTL_MOD, fd, &event) != -1 ||
         (errno == ENOENT &&
          epoll_ctl(io_fd_, EPOLL_CTL_ADD, fd, &event) != -1);
}

}  // namespace net
}  // namespace dist_clang
