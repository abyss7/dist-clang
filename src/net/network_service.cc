#include "net/network_service.h"

#include "base/c_utils.h"
#include "net/base/utils.h"
#include "net/epoll_event_loop.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkService::NetworkService(size_t concurrency)
  : epoll_fd_(epoll_create1(EPOLL_CLOEXEC)), concurrency_(concurrency) {
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new EpollEventLoop(callback));
}

bool NetworkService::Run() {
  using namespace std::placeholders;

  auto old_signals = BlockSignals();

  pool_.reset(new WorkerPool);
  auto work = std::bind(&NetworkService::DoConnectWork, this, _1);
  pool_->AddWorker(work, concurrency_);

  UnblockSignals(old_signals);

  return event_loop_->Run();
}

NetworkService::~NetworkService() {
  pool_.reset();
  close(epoll_fd_);
}

bool NetworkService::Listen(const std::string& path,
                            ListenCallback callback,
                            std::string* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  auto socket_address = reinterpret_cast<sockaddr*>(&address);
  if (bind(fd, socket_address, sizeof(address)) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }

  if (listen(fd, 100) == -1) {  // FIXME: hardcode.
    base::GetLastError(error);
    close(fd);
    return false;
  }

  if (!listen_callbacks_.insert(std::make_pair(fd, callback)).second) {
    close(fd);
    return false;
  }

  if (!static_cast<EpollEventLoop*>(event_loop_.get())->HandlePassive(fd)) {
    close(fd);
    return false;
  }

  return true;
}

bool NetworkService::Listen(const std::string &host, unsigned short port,
                            ListenCallback callback,
                            std::string* error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    base::GetLastError(error);
    return false;
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  auto fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }

  auto socket_address = reinterpret_cast<sockaddr*>(&address);
  if (bind(fd, socket_address, sizeof(address)) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }

  if (listen(fd, 100) == -1) {  // FIXME: hardcode.
    base::GetLastError(error);
    close(fd);
    return false;
  }

  if (!listen_callbacks_.insert(std::make_pair(fd, callback)).second) {
    close(fd);
    return false;
  }

  if (!static_cast<EpollEventLoop*>(event_loop_.get())->HandlePassive(fd)) {
    close(fd);
    return false;
  }

  return true;
}

ConnectionPtr NetworkService::ConnectSync(const std::string &path,
                                      std::string *error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return ConnectionPtr();
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  auto connection =
      static_cast<EpollEventLoop*>(event_loop_.get())->HandleActive(fd);
  if (!connection) {
    close(fd);
  }

  return connection;
}

ConnectionPtr NetworkService::ConnectSync(const std::string &host,
                                      unsigned short port,
                                      std::string *error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    base::GetLastError(error);
    return ConnectionPtr();
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  auto fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return ConnectionPtr();
  }

  auto socket_address = reinterpret_cast<sockaddr*>(&address);
  if (connect(fd, socket_address, sizeof(address)) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  auto connection =
      static_cast<EpollEventLoop*>(event_loop_.get())->HandleActive(fd);
  if (!connection) {
    close(fd);
  }

  return connection;
}

bool NetworkService::ConnectAsync(const std::string &host, unsigned short port,
                                  ConnectCallback callback,
                                  std::string *error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    base::GetLastError(error);
    return false;
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  auto fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  auto socket_address = reinterpret_cast<sockaddr*>(&address);
  if (connect(fd, socket_address, sizeof(address)) == -1) {
    if (errno != EINPROGRESS) {
      base::GetLastError(error);
      close(fd);
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(connect_mutex_);
    connect_callbacks_.insert(std::make_pair(fd, callback));
  }

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }

  return true;
}

void NetworkService::HandleNewConnection(fd_t fd, ConnectionPtr connection) {
  auto callback = listen_callbacks_.find(fd);
  assert(callback != listen_callbacks_.end());
  callback->second(connection);
}

void NetworkService::DoConnectWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];
  sigset_t signal_set;
  unsigned long error;
  unsigned int error_size = sizeof(error);

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count =
        epoll_pwait(epoll_fd_, events, MAX_EVENTS, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      assert(events[i].events & EPOLLOUT);
      fd_t fd = events[i].data.fd;
      ConnectCallback callback;
      {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        auto it = connect_callbacks_.find(fd);
        if (it != connect_callbacks_.end()) {
          callback = it->second;
          connect_callbacks_.erase(it);
        }
      }

      assert(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != -1);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == -1) {
        std::string error;
        base::GetLastError(&error);
        if (callback) {
          callback(ConnectionPtr(), error);
        }
        close(fd);
      }
      else if (error) {
        errno = error;
        std::string error;
        base::GetLastError(&error);
        if (callback) {
          callback(ConnectionPtr(), error);
        }
        close(fd);
      }
      else {
        MakeNonBlocking(fd, true);
        auto connection =
            static_cast<EpollEventLoop*>(event_loop_.get())->HandleActive(fd);
        if (!connection) {
          close(fd);
        }
        if (callback) {
          callback(connection, std::string());
        }
      }
    }
  }
}

}  // namespace net
}  // namespace dist_clang
