#include <net/network_service_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file_utils.h>
#include <net/connection_impl.h>
#include <net/end_point.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace dist_clang {
namespace net {

NetworkServiceImpl::~NetworkServiceImpl() {
  for (const auto& socket : unix_sockets_) {
    base::DeleteFile(socket);
  }
}

bool NetworkServiceImpl::Run() {
  return event_loop_->Run();
}

bool NetworkServiceImpl::Listen(const String& path, ListenCallback callback,
                                String* error) {
  auto peer = EndPoint::UnixSocket(path);
  if (!peer) {
    return false;
  }
  unlink(path.c_str());

  auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  if (::bind(fd, *peer, peer->size()) == -1) {
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

  unix_sockets_.push_back(path);
  return true;
}

bool NetworkServiceImpl::Listen(const String& host, ui16 port, bool ipv6,
                                ListenCallback callback, String* error) {
  auto peer = EndPoint::LocalHost(host, port, ipv6);
  if (!peer) {
    return false;
  }

  auto fd = socket(peer->domain(), peer->type() | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   peer->protocol());
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

  if (::bind(fd, *peer, peer->size()) == -1) {
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
  auto fd = socket(end_point->domain(), end_point->type() | SOCK_CLOEXEC,
                   end_point->protocol());
  if (fd == -1) {
    base::GetLastError(error);
    return ConnectionPtr();
  }

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

  timeout = {read_timeout_secs, 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, timeout_size) == -1) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  auto low_watermark = read_min_bytes;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &low_watermark,
                 sizeof(low_watermark))) {
    base::GetLastError(error);
    close(fd);
    return ConnectionPtr();
  }

  return ConnectionImpl::Create(*event_loop_, fd, end_point);
}

void NetworkServiceImpl::HandleNewConnection(FileDescriptor fd,
                                             ConnectionPtr connection) {
  auto callback = listen_callbacks_.find(fd);
  DCHECK(callback != listen_callbacks_.end());

  struct timeval timeout = {send_timeout_secs, 0};
  constexpr auto timeout_size = sizeof(timeout);
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, timeout_size) == -1) {
    callback->second(ConnectionPtr());
  }

  timeout = {read_timeout_secs, 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, timeout_size) == -1) {
    callback->second(ConnectionPtr());
  }

  callback->second(connection);
}

}  // namespace net
}  // namespace dist_clang
