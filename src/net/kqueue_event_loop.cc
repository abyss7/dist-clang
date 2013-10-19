#include "net/kqueue_event_loop.h"

#include "base/assert.h"
#include "net/base/utils.h"

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

bool KqueueEventLoop::HandlePassive(fd_t fd) {
#if !defined(OS_MACOSX)
  // TODO: don't know why, but assertion fails on Mac.
  base::Assert(IsListening(fd));
#endif
  base::Assert(IsNonBlocking(fd));
  listening_fds_.insert(fd);
  return ReadyForListen(fd);
}

bool KqueueEventLoop::ReadyForRead(ConnectionPtr connection) {
  return ReadyFor(connection, EVFILT_READ);
}

bool KqueueEventLoop::ReadyForSend(ConnectionPtr connection) {
  return ReadyFor(connection, EVFILT_WRITE);
}

void KqueueEventLoop::RemoveConnection(fd_t fd) {
  struct kevent events[2];
  EV_SET(events + 0, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
  EV_SET(events + 1, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
  kevent(io_fd_, events, 2, nullptr, 0, nullptr);
  base::WriteLock lock(connections_mutex_);
  connections_.erase(fd);
}

void KqueueEventLoop::DoListenWork(const volatile bool &is_shutting_down,
                                   fd_t self_pipe) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct kevent events[MAX_EVENTS];

  {
    struct kevent event;
    EV_SET(&event, self_pipe, EVFILT_READ, EV_ADD, 0, 0, 0);
    kevent(listen_fd_, &event, 1, nullptr, 0, nullptr);
  }

  while(!is_shutting_down) {
  	auto events_count =
        kevent(listen_fd_, nullptr, 0, events, MAX_EVENTS, nullptr);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      fd_t fd = events[i].ident;
      if (fd == self_pipe) {
        continue;
      }

      base::Assert(events[i].filter == EVFILT_READ);
      while(true) {
        auto new_fd = accept(fd, nullptr, nullptr);
        if (new_fd == -1) {
          base::Assert(errno == EAGAIN || errno == EWOULDBLOCK);
          break;
        }
        MakeCloseOnExec(new_fd);
        MakeNonBlocking(new_fd, true);
        callback_(fd, Connection::Create(*this, new_fd));
      }
      ReadyForListen(fd);
    }
  }

  for (auto fd: listening_fds_)
    close(fd);
}

void KqueueEventLoop::DoIOWork(const volatile bool& is_shutting_down,
                               fd_t self_pipe) {
  struct kevent event;

  {
    struct kevent event;
    EV_SET(&event, self_pipe, EVFILT_READ, EV_ADD, 0, 0, 0);
    kevent(listen_fd_, &event, 1, nullptr, 0, nullptr);
  }

  while(!is_shutting_down) {
    auto events_count = kevent(io_fd_, nullptr, 0, &event, 1, nullptr);
    if (events_count == -1) {
      if (errno != EINTR) {
        break;
      }
      else {
        continue;
      }
    }

    fd_t fd = event.ident;
    if (fd == self_pipe) {
      continue;
    }

    net::ConnectionPtr connection;
    base::Assert(events_count == 1);
    {
      base::ReadLock lock(connections_mutex_);
      connection = connections_[fd].lock();
    }

    if (event.flags & (EV_EOF|EV_ERROR)) {
      struct kevent events[2];
      EV_SET(events + 0, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
      EV_SET(events + 1, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
      base::Assert(!kevent(io_fd_, events, 2, nullptr, 0, nullptr));
    }

    if (connection) {
      if (event.filter == EVFILT_READ) {
        ConnectionDoRead(connection);
      }
      else if (event.filter == EVFILT_WRITE) {
        ConnectionDoSend(connection);
      }
    }
  }
}

bool KqueueEventLoop::ReadyForListen(fd_t fd) {
  struct kevent event;
  EV_SET(&event, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, 0);
  return kevent(listen_fd_, &event, 1, nullptr, 0, nullptr) != -1;
}

bool KqueueEventLoop::ReadyFor(ConnectionPtr connection, int16_t filter) {
  base::Assert(connection->IsOnEventLoop(this));

  auto fd = GetConnectionDescriptor(connection);
  struct kevent event;
  EV_SET(&event, fd, filter, EV_ENABLE | EV_ONESHOT, 0, 0, 0);
  base::WriteLock lock(connections_mutex_);
  if (kevent(io_fd_, &event, 1, nullptr, 0, nullptr) == -1) {
    if (errno != ENOENT || !ConnectionAdd(connection)) {
      return false;
    }
    event.flags |= EV_ADD;
    if (kevent(io_fd_, &event, 1, nullptr, 0, nullptr) == -1) {
      return false;
    }
    connections_.insert(std::make_pair(fd, connection));
  }
  return true;
}

}  // namespace net
}  // namespace dist_clang
