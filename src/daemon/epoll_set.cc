#include "daemon/epoll_set.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

EpollSet::EpollSet()
  : epoll_fd_(epoll_create1(0)) {}

EpollSet::~EpollSet() {
  for (auto it = fds_.begin(), end = fds_.end(); it != end; ++it) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, *it, nullptr);
    close(*it);
  }
  close(epoll_fd_);
}

bool EpollSet::HandleSocket(int fd) {
  int result;
  socklen_t size = sizeof(result);

  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &result, &size) == -1)
    return false;

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = fd;

  MakeNonBlocking(fd);

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1)
    return false;

  fds_.insert(fd);
  return true;
}

bool EpollSet::RearmSocket(int fd) {
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.fd = fd;
  return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) != -1;
}

int EpollSet::Wait(std::vector<Event>& events) {
  const size_t max_events = 10;  // FIXME: hardcode.
  struct epoll_event epoll_events[max_events];

  auto number_of_events = epoll_wait(epoll_fd_, epoll_events, max_events, -1);
  if (number_of_events == -1)
    return -1;

  int real_events = 0;
  for (int i = 0; i < number_of_events; ++i) {
    bool close_after_use = epoll_events[i].events & EPOLLHUP;
    auto fd = epoll_events[i].data.fd;

    if (epoll_events[i].events & EPOLLIN) {
      if (IsListening(fd)) {
        auto new_connection = accept(fd, nullptr, nullptr);
        if (!HandleSocket(new_connection))
          close(new_connection);

        if (!close_after_use)
          RearmSocket(fd);
        else {
          fds_.erase(fd);
          epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
          close(fd);
        }
      } else {
        Event event;
        event.close_after_use = close_after_use;
        event.fd = fd;

        events.push_back(event);
        ++real_events;
        if (close_after_use) {
          fds_.erase(fd);
          epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        }
      }
    } else if (close_after_use) {
      fds_.erase(fd);
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      close(fd);
    }
  }

  return real_events;
}

bool EpollSet::MakeNonBlocking(int fd) {
  int flags, s;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return false;

  flags |= O_NONBLOCK;
  s = fcntl(fd, F_SETFL, flags);
  if (s == -1)
    return false;

  return true;
}

bool EpollSet::IsListening(int fd) {
  int result;
  socklen_t size = sizeof(result);

  return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &result, &size) != -1 &&
      result;
}
