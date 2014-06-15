#include <net/network_service_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <net/base/end_point.h>
#include <net/base/utils.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

using namespace ::std::placeholders;

namespace dist_clang {
namespace net {

bool NetworkServiceImpl::Run() { return event_loop_->Run(); }

bool NetworkServiceImpl::Listen(const String& path, ListenCallback callback,
                                String* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  unlink(path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }
  MakeCloseOnExec(fd);
  MakeNonBlocking(fd);

  auto socket_address = reinterpret_cast<sockaddr*>(&address);
  if (bind(fd, socket_address, sizeof(address)) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }
  base::SetPermissions(path, 0777);

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

bool NetworkServiceImpl::Listen(const String& host, ui16 port,
                                ListenCallback callback, String* error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    base::GetLastError(error);
    return false;
  }

  address_list = reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  auto fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }
  MakeCloseOnExec(fd);
  MakeNonBlocking(fd);

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

ConnectionPtr NetworkServiceImpl::Connect(EndPointPtr end_point,
                                          String* error) {
  auto fd = socket(end_point->domain(), SOCK_STREAM, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return ConnectionPtr();
  }
  MakeCloseOnExec(fd);

  if (connect(fd, *end_point, end_point->size()) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  struct timeval timeout = {send_timeout_secs, 0};
  constexpr auto timeout_size = sizeof(timeout);
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, timeout_size) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  return ConnectionImpl::Create(*event_loop_, fd, end_point);
}

void NetworkServiceImpl::HandleNewConnection(fd_t fd,
                                             ConnectionPtr connection) {
  auto callback = listen_callbacks_.find(fd);
  DCHECK(callback != listen_callbacks_.end());

  struct timeval timeout = {send_timeout_secs, 0};
  constexpr auto timeout_size = sizeof(timeout);
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, timeout_size) == -1) {
    callback->second(ConnectionPtr());
  } else {
    callback->second(connection);
  }
}

}  // namespace net
}  // namespace dist_clang
