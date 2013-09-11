#include "net/epoll_event_loop.h"

#include "net/connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool IsListening(int fd) {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

}  // namespace

namespace dist_clang {
namespace net {

EpollEventLoop::EpollEventLoop(ConnectionCallback callback)
  : listen_fd_(epoll_create1(0)), incoming_fd_(epoll_create1(0)),
    outgoing_fd_(epoll_create1(0)), callback_(callback) {}

bool EpollEventLoop::Handle(int fd) {
  if (IsListening(fd)) {
    std::unique_lock<std::mutex> lock(listening_fds_mutex_);
    listening_fds_.insert(fd);
    return ReadyForListen(fd);
  } else {
    AddConnection(fd);
    return true;
  }
}

bool EpollEventLoop::ReadyForRead(ConnectionPtr connection) {
  assert(connection->IsOnEventLoop(this));

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
  event.data.ptr = connection.get();
  auto fd = GetConnectionDescriptor(connection);
  if (epoll_ctl(incoming_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno == ENOENT)
      return epoll_ctl(incoming_fd_, EPOLL_CTL_ADD, fd, &event) != 1;
    return false;
  }
  return true;
}

bool EpollEventLoop::ReadyForSend(ConnectionPtr connection) {
  assert(connection->IsOnEventLoop(this));

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLRDHUP | EPOLLONESHOT;
  event.data.ptr = connection.get();
  auto fd = GetConnectionDescriptor(connection);
  if (epoll_ctl(outgoing_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
    if (errno == ENOENT)
      return epoll_ctl(outgoing_fd_, EPOLL_CTL_ADD, fd, &event) != -1;
    return false;
  }
  return true;
}

void EpollEventLoop::DoListenWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  const int TIMEOUT = 3 * 1000;  // In milliseconds.
  struct epoll_event events[MAX_EVENTS];

  while(!is_shutting_down) {
    auto events_count = epoll_wait(listen_fd_, events, MAX_EVENTS, TIMEOUT);
    if (events_count == -1)
      break;

    for (int i = 0; i < events_count; ++i) {
      auto fd = events[i].data.fd;

      if (events[i].events & EPOLLHUP) {
        epoll_ctl(listen_fd_, EPOLL_CTL_DEL, fd, nullptr);
        listening_fds_.erase(fd);
        close(fd);
      } else if (events[i].events & EPOLLIN) {
        auto new_fd = accept(fd, nullptr, nullptr);
        auto new_connection = AddConnection(new_fd);
        ReadyForListen(fd);
        callback_(fd, new_connection);
      }
    }

    std::unique_lock<std::mutex> lock(connections_mutex_);
    for (auto connection: connections_) {  // FIXME: is it safe to delete?
      if (connection->IsClosed()) {
        auto fd = GetConnectionDescriptor(connection);
        epoll_ctl(incoming_fd_, EPOLL_CTL_DEL, fd, nullptr);
        epoll_ctl(outgoing_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(connection);
      }
    }
  }

  std::unique_lock<std::mutex> lock(listening_fds_mutex_);
  for (auto fd: listening_fds_)
    close(fd);
}

void EpollEventLoop::DoIncomingWork(const volatile bool& is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  const int TIMEOUT = 3 * 1000;  // In milliseconds.
  struct epoll_event events[MAX_EVENTS];

  while(!is_shutting_down) {
    auto events_count = epoll_wait(incoming_fd_, events, MAX_EVENTS, TIMEOUT);
    if (events_count == -1)
      return;

    for (int i = 0; i < events_count; ++i) {
      auto connection =
          reinterpret_cast<Connection*>(events[i].data.ptr)->shared_from_this();

      if (events[i].events & EPOLLRDHUP)
        connection->Close();
      else if (events[i].events & EPOLLIN)
        ConnectionCanRead(connection);
    }
  }
}

void EpollEventLoop::DoOutgoingWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  const int TIMEOUT = 3 * 1000;  // In milliseconds.
  struct epoll_event events[MAX_EVENTS];

  while(!is_shutting_down) {
    auto events_count = epoll_wait(outgoing_fd_, events, MAX_EVENTS, TIMEOUT);
    if (events_count == -1)
      return;

    for (int i = 0; i < events_count; ++i) {
      auto connection =
          reinterpret_cast<Connection*>(events[i].data.ptr)->shared_from_this();

      if (events[i].events & EPOLLRDHUP)
        connection->Close();
      else if (events[i].events & EPOLLOUT)
        ConnectionCanSend(connection);
    }
  }
}

bool EpollEventLoop::ReadyForListen(int fd) {
  assert(listening_fds_.count(fd));

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

ConnectionPtr EpollEventLoop::AddConnection(int fd) {
  std::unique_lock<std::mutex> lock(connections_mutex_);

  ConnectionPtr new_connection = Connection::Create(*this, fd);
  connections_.insert(new_connection);
  return new_connection;
}

}  // namespace net
}  // namespace dist_clang
