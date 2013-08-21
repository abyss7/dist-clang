#include "server.h"

#include "base/c_utils.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>

using std::string;

namespace {

bool make_non_blocking (int fd) {
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

}  // namespace

Server::Server(size_t concurrency)
  : concurrency_(concurrency), thread_pool_(1024, concurrency),
    network_threads_(concurrency) {}

bool Server::Listen(const string& path, string* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    GetLastError(error);
    return false;
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    GetLastError(error);
    return false;
  }

  make_non_blocking(fd);

  // FIXME: what number to use here?
  if (listen(fd, concurrency_) == -1) {
    GetLastError(error);
    return false;
  }

  listen_fds_.insert(fd);

  return true;
}

bool Server::Listen(const string& host, short int port, string* error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    GetLastError(error);
    return false;
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    GetLastError(error);
    return false;
  }

  make_non_blocking(fd);

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    GetLastError(error);
    return false;
  }

  // FIXME: what number to use here?
  if (listen(fd, concurrency_) == -1) {
    GetLastError(error);
    return false;
  }

  listen_fds_.insert(fd);

  return true;
}

bool Server::Run() {
  int fd = epoll_create1(0);
  if (fd == -1)
    return false;

  struct epoll_event ev;
  ev.events = EPOLLIN;

  for (auto it = listen_fds_.begin(); it != listen_fds_.end(); ++it) {
    if (epoll_ctl(fd, EPOLL_CTL_ADD, *it, &ev) == -1)
      return false;
  }

  auto lambda = [this, &fd](std::thread& thread) {
    std::thread tmp(&Server::DoWork, this, fd);
    thread.swap(tmp);
  };
  std::for_each(network_threads_.begin(), network_threads_.end(), lambda);

  return true;
}

void Server::DoWork(int fd) {
  const size_t max_events = 10;
  struct epoll_event events[max_events], event;

  while (true) {
    auto number_of_events = epoll_wait(fd, events, max_events, -1);
    if (number_of_events == -1)
      return;

    for (int i = 0; i < number_of_events; ++i) {
      if (listen_fds_.count(events[i].data.fd)) {
        auto new_connection = accept(events[i].data.fd, nullptr, nullptr);
        if (new_connection == -1)
          continue;
        make_non_blocking(new_connection);
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = new_connection;
        if (epoll_ctl(fd, EPOLL_CTL_ADD, new_connection, &event) == -1) {
          close(new_connection);
          continue;
        }
      }
    }
  }
}
