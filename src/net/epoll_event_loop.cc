#include "net/epoll_event_loop.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "net/base/utils.h"
#include "net/connection.h"

#include <sys/epoll.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

EpollEventLoop::EpollEventLoop(ConnectionCallback callback)
  : listen_fd_(epoll_create1(EPOLL_CLOEXEC)),
    io_fd_(epoll_create1(EPOLL_CLOEXEC)),
    callback_(callback) {}

EpollEventLoop::~EpollEventLoop() {
  Stop();
  close(listen_fd_);
  close(io_fd_);
}

bool EpollEventLoop::HandlePassive(fd_t fd) {
  base::Assert(IsListening(fd));
  base::Assert(IsNonBlocking(fd));
  listening_fds_.insert(fd);
  return ReadyForListen(fd);
}

bool EpollEventLoop::ReadyForRead(ConnectionPtr connection) {
  base::Assert(connection->IsOnEventLoop(this));

  auto fd = GetConnectionDescriptor(connection);
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = fd;
  if (epoll_ctl(io_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno != ENOENT ||
        !ConnectionAdd(connection) ||
        epoll_ctl(io_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
      return false;
    }
    base::WriteLock lock(connections_mutex_);
    connections_.insert(std::make_pair(fd, connection));
  }
  return true;
}

bool EpollEventLoop::ReadyForSend(ConnectionPtr connection) {
  base::Assert(connection->IsOnEventLoop(this));

  auto fd = GetConnectionDescriptor(connection);
  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.fd = fd;
  if(epoll_ctl(io_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno != ENOENT ||
        !ConnectionAdd(connection) ||
        epoll_ctl(io_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
      return false;
    }
    base::WriteLock lock(connections_mutex_);
    connections_.insert(std::make_pair(fd, connection));
  }
  return true;
}

void EpollEventLoop::RemoveConnection(fd_t fd) {
  epoll_ctl(io_fd_, EPOLL_CTL_DEL, fd, nullptr);
  base::WriteLock lock(connections_mutex_);
  connections_.erase(fd);
}

void EpollEventLoop::DoListenWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];
  sigset_t signal_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count =
        epoll_pwait(listen_fd_, events, MAX_EVENTS, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      base::Assert(events[i].events & EPOLLIN);
      auto fd = events[i].data.fd;
      while(true) {
        auto new_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (new_fd == -1) {
          base::Assert(errno == EAGAIN || errno == EWOULDBLOCK);
          break;
        }
        callback_(fd, Connection::Create(*this, new_fd));
      }
      ReadyForListen(fd);
    }
  }

  for (auto fd: listening_fds_)
    close(fd);
}

void EpollEventLoop::DoIOWork(const volatile bool& is_shutting_down) {
  struct epoll_event event;
  sigset_t signal_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count = epoll_pwait(io_fd_, &event, 1, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    net::ConnectionPtr connection;
    base::Assert(events_count == 1);
    {
      base::ReadLock lock(connections_mutex_);
      connection = connections_[event.data.fd].lock();
    }

    if (event.events & (EPOLLHUP|EPOLLERR)) {
      base::Assert(!epoll_ctl(io_fd_, EPOLL_CTL_DEL, event.data.fd, nullptr));
    }

    if (connection) {
      if (event.events & EPOLLIN) {
        ConnectionDoRead(connection);
      }
      else if (event.events & EPOLLOUT) {
        ConnectionDoSend(connection);
      }
    }
  }
}

bool EpollEventLoop::ReadyForListen(fd_t fd) {
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = fd;
  if (epoll_ctl(listen_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno == ENOENT)
      return epoll_ctl(listen_fd_, EPOLL_CTL_ADD, fd, &event) != -1;
    return false;
  }
  return true;
}

}  // namespace net
}  // namespace dist_clang
