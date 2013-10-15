#include "net/network_service.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "net/base/end_point.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

using ::std::string;

namespace dist_clang {
namespace net {

NetworkService::NetworkService(size_t concurrency)
  : concurrency_(concurrency) {
}

NetworkService::~NetworkService() {
  pool_.reset();
  close(epoll_fd_);
}

bool NetworkService::Listen(const string& path, ListenCallback callback,
                            string* error) {
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

  if (!event_loop_->HandlePassive(fd)) {
    close(fd);
    return false;
  }

  return true;
}

bool NetworkService::Listen(const string &host, unsigned short port,
                            ListenCallback callback, string* error) {
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

  if (!event_loop_->HandlePassive(fd)) {
    close(fd);
    return false;
  }

  return true;
}

ConnectionPtr NetworkService::ConnectSync(const string &path, string *error) {
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

  return Connection::Create(*event_loop_, fd);
}

ConnectionPtr NetworkService::ConnectSync(EndPointPtr end_point,
                                          string *error) {
  auto fd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return ConnectionPtr();
  }

  if (connect(fd, *end_point, end_point->size()) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  return Connection::Create(*event_loop_, fd, end_point);
}

void NetworkService::HandleNewConnection(fd_t fd, ConnectionPtr connection) {
  auto callback = listen_callbacks_.find(fd);
  base::Assert(callback != listen_callbacks_.end());
  callback->second(connection);
}

}  // namespace net
}  // namespace dist_clang
