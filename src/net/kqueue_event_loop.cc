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
  DCHECK(IsListening(fd));
#endif
  DCHECK(IsNonBlocking(fd));
  listening_fds_.insert(fd);
  return ReadyForListen(fd);
}

bool KqueueEventLoop::ReadyForRead(ConnectionPtr connection) {
  return ReadyFor(connection, EVFILT_READ);
}

bool KqueueEventLoop::ReadyForSend(ConnectionPtr connection) {
  return ReadyFor(connection, EVFILT_WRITE);
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

      DCHECK(events[i].filter == EVFILT_READ);
      while(true) {
        auto new_fd = accept(fd, nullptr, nullptr);
        if (new_fd == -1) {
          DCHECK(errno == EAGAIN || errno == EWOULDBLOCK);
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
  EV_SET(&event, self_pipe, EVFILT_READ, EV_ADD, 0, 0, 0);
  kevent(io_fd_, &event, 1, nullptr, 0, nullptr);

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

    DCHECK(events_count == 1);
    fd_t fd = event.ident;

    if (fd == self_pipe) {
      continue;
    }

    auto raw_connection = reinterpret_cast<Connection*>(event.udata);
    auto connection = raw_connection->shared_from_this();

    if (event.flags & EV_ERROR || (event.flags & EV_EOF && event.data == 0)) {
      ConnectionClose(connection);
    }
    else if (event.filter == EVFILT_READ) {
      ConnectionDoRead(connection);
    }
    else if (event.filter == EVFILT_WRITE) {
      ConnectionDoSend(connection);
    }
    else {
      NOTREACHED();
    }
  }
}

bool KqueueEventLoop::ReadyForListen(fd_t fd) {
  struct kevent event;
  EV_SET(&event, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, 0);
  return kevent(listen_fd_, &event, 1, nullptr, 0, nullptr) != -1;
}

bool KqueueEventLoop::ReadyFor(ConnectionPtr connection, int16_t filter) {
  DCHECK(connection->IsOnEventLoop(this));

  auto fd = GetConnectionDescriptor(connection);
  struct kevent event;
  EV_SET(&event, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, connection.get());
  return kevent(io_fd_, &event, 1, nullptr, 0, nullptr) != -1;
}

}  // namespace net
}  // namespace dist_clang
