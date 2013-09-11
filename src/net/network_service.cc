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
  // TODO: implement this.
  return false;
}

bool NetworkService::Listen(const string& path,
                            ConnectionCallback callback,
                            string* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) == -1) {
    base::GetLastError(error);
    return false;
  }

  if (listen(fd, 100) == -1) {  // FIXME: hardcode.
    base::GetLastError(error);
    return false;
  }

  if (!event_loop_->Handle(fd))
    return false;

  return true;
}

void NetworkService::HandleNewConnection(int fd, ConnectionPtr connection) {
  // TODO: implement this.
}

}  // namespace net
}  // namespace dist_clang
