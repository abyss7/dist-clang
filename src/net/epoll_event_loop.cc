#include "net/epoll_event_loop.h"

#include "net/base/utils.h"
#include "net/connection.h"

#include <sys/epoll.h>
#include <unistd.h>

namespace dist_clang {
namespace net {

EpollEventLoop::EpollEventLoop(ConnectionCallback callback)
  : listen_fd_(epoll_create1(EPOLL_CLOEXEC)),
    io_fd_(epoll_create1(EPOLL_CLOEXEC)),
    closing_fd_(epoll_create1(EPOLL_CLOEXEC)), callback_(callback) {}

EpollEventLoop::~EpollEventLoop() {
  Stop();
  close(listen_fd_);
  close(io_fd_);
  close(closing_fd_);
}

bool EpollEventLoop::HandlePassive(fd_t fd) {
  assert(IsListening(fd));
  assert(IsNonBlocking(fd));
  std::unique_lock<std::mutex> lock(listening_fds_mutex_);
  listening_fds_.insert(fd);
  return ReadyForListen(fd);
}

ConnectionPtr EpollEventLoop::HandleActive(fd_t fd) {
  assert(!IsListening(fd));
  ConnectionPtr new_connection = Connection::Create(*this, fd);
  std::unique_lock<std::mutex> lock(connections_mutex_);
  connections_.insert(new_connection);
  if (!ReadyForClose(new_connection))
    return ConnectionPtr();
  return new_connection;
}

bool EpollEventLoop::ReadyForRead(ConnectionPtr connection) {
  assert(connection->IsOnEventLoop(this));

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.ptr = connection.get();
  auto fd = GetConnectionDescriptor(connection);
  if (!ConnectionToggleWait(connection, true)) {
    return false;
  }
  if (epoll_ctl(io_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    assert(errno == ENOENT);
    if (!ConnectionAdd(connection) ||
        epoll_ctl(io_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
      ConnectionToggleWait(connection, false);
      return false;
    }
  }
  return true;
}

bool EpollEventLoop::ReadyForSend(ConnectionPtr connection) {
  assert(connection->IsOnEventLoop(this));

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.ptr = connection.get();
  auto fd = GetConnectionDescriptor(connection);
  if (!ConnectionToggleWait(connection, true)) {
    return false;
  }
  if(epoll_ctl(io_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    assert(errno == ENOENT);
    if (!ConnectionAdd(connection) ||
        epoll_ctl(io_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
      ConnectionToggleWait(connection, false);
      return false;
    }
  }
  return true;
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
      assert(events[i].events & EPOLLIN);
      auto fd = events[i].data.fd;
      while(true) {
        auto new_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (new_fd == -1) {
          assert(errno == EAGAIN || errno == EWOULDBLOCK);
          break;
        }
        auto new_connection = HandleActive(new_fd);
        callback_(fd, new_connection);
      }
      ReadyForListen(fd);
    }
  }

  std::unique_lock<std::mutex> lock(listening_fds_mutex_);
  for (auto fd: listening_fds_)
    close(fd);
}

void EpollEventLoop::DoIOWork(const volatile bool& is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];
  sigset_t signal_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count =
        epoll_pwait(io_fd_, events, MAX_EVENTS, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      auto connection =
          reinterpret_cast<Connection*>(events[i].data.ptr)->shared_from_this();

      if (events[i].events & (EPOLLHUP|EPOLLERR)) {
        auto fd = GetConnectionDescriptor(connection);
        assert(!epoll_ctl(io_fd_, EPOLL_CTL_DEL, fd, nullptr));
      }
      assert(ConnectionToggleWait(connection, false));

      if (events[i].events & EPOLLIN) {
        ConnectionDoRead(connection);
      }
      else if (events[i].events & EPOLLOUT) {
        ConnectionDoSend(connection);
      }
    }
  }
}

void EpollEventLoop::DoClosingWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];
  sigset_t signal_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count =
        epoll_pwait(closing_fd_, events, MAX_EVENTS, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      return;
    }

    for (int i = 0; i < events_count; ++i) {
      assert(events[i].events & (EPOLLHUP|EPOLLRDHUP|EPOLLERR));
      auto connection =
          reinterpret_cast<Connection*>(events[i].data.ptr)->shared_from_this();
      {
        std::unique_lock<std::mutex> lock(connections_mutex_);
        connections_.erase(connection);
      }

      auto fd = GetConnectionDescriptor(connection);
      if (ConnectionToggleWait(connection, true)) {
        epoll_ctl(io_fd_, EPOLL_CTL_DEL, fd, nullptr);
      }
      else {
        pending_connections_.insert(connection);
      }
    }

    for (auto it = pending_connections_.begin();
         it != pending_connections_.end();) {
      if (ConnectionToggleWait(*it, true)) {
        it = pending_connections_.erase(it);
      }
      else {
        ++it;
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

bool EpollEventLoop::ReadyForClose(ConnectionPtr connection) {
  assert(connection->IsOnEventLoop(this));

  struct epoll_event event;

  event.events = EPOLLONESHOT | EPOLLRDHUP;
  event.data.ptr = connection.get();
  auto fd = GetConnectionDescriptor(connection);
  if (epoll_ctl(closing_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    return false;
  }
  return true;
}

}  // namespace net
}  // namespace dist_clang
