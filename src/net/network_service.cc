#include "net/network_service.h"

#include "base/c_utils.h"
#include "net/epoll_event_loop.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

using std::string;
using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkService::NetworkService()
  : event_loop_(new EpollEventLoop(std::bind(
      &NetworkService::HandleNewConnection, this, _1, _2))) {}

bool NetworkService::Run() {
  return event_loop_->Run();
}

bool NetworkService::Listen(const string& path,
                            ConnectionCallback callback,
                            proto::Error *error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    if (error) {
      error->set_code(proto::Error::NETWORK);
      base::GetLastError(error->mutable_description());
    }
    return false;
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    if (error) {
      error->set_code(proto::Error::NETWORK);
      base::GetLastError(error->mutable_description());
    }
    close(fd);
    return false;
  }

  if (listen(fd, 100) == -1) {  // FIXME: hardcode.
    if (error) {
      error->set_code(proto::Error::NETWORK);
      base::GetLastError(error->mutable_description());
    }
    close(fd);
    return false;
  }

  if (!callbacks_.insert(std::make_pair(fd, callback)).second) {
    close(fd);
    return false;
  }

  if (!static_cast<EpollEventLoop*>(event_loop_.get())->HandlePassive(fd)) {
    close(fd);
    return false;
  }

  return true;
}

ConnectionPtr NetworkService::Connect(const std::string &path,
                                      proto::Error *error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    if (error) {
      error->set_code(proto::Error::NETWORK);
      base::GetLastError(error->mutable_description());
    }
    return ConnectionPtr();
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == -1) {
    if (error) {
      error->set_code(proto::Error::NETWORK);
      base::GetLastError(error->mutable_description());
    }
    close(fd);
    return ConnectionPtr();
  }

  auto connection =
      static_cast<EpollEventLoop*>(event_loop_.get())->HandleActive(fd);
  if (!connection) {
    close(fd);
    return ConnectionPtr();
  }

  return connection;
}

void NetworkService::HandleNewConnection(fd_t fd, ConnectionPtr connection) {
  auto callback = callbacks_.find(fd);
  assert(callback != callbacks_.end());
  callback->second(connection);
}

}  // namespace net
}  // namespace dist_clang
