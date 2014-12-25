#include <net/network_service_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file_utils.h>
#include <net/connection_impl.h>
#include <net/end_point.h>
#include <net/socket.h>

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

  Socket fd(peer);
  if (!fd.IsValid()) {
    base::GetLastError(error);
    return false;
  }

  fd.CloseOnExec();
  fd.MakeBlocking(false);

  if (!fd.Bind(peer, error)) {
    return false;
  }
  base::SetPermissions(path, 0777);

  Passive passive(std::move(fd));
  if (!passive.IsValid()) {
    passive.GetCreationError(error);
    return false;
  }

  if (!listen_callbacks_.emplace(passive.native(), callback).second) {
    return false;
  }

  if (!event_loop_->HandlePassive(std::move(passive))) {
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

  Socket fd(peer);
  if (!fd.IsValid()) {
    base::GetLastError(error);
    return false;
  }

  fd.CloseOnExec();
  fd.MakeBlocking(false);

  if (!fd.ReuseAddress(error)) {
    return false;
  }

  if (!fd.Bind(peer, error)) {
    return false;
  }

  Passive passive(std::move(fd));
  if (!passive.IsValid()) {
    passive.GetCreationError(error);
    return false;
  }

  if (!listen_callbacks_.emplace(passive.native(), callback).second) {
    return false;
  }

  if (!event_loop_->HandlePassive(std::move(passive))) {
    return false;
  }

  return true;
}

ConnectionPtr NetworkServiceImpl::Connect(EndPointPtr end_point,
                                          String* error) {
  Socket fd(end_point);
  if (!fd.IsValid()) {
    base::GetLastError(error);
    return ConnectionPtr();
  }

  fd.CloseOnExec();

  if (!fd.Connect(end_point, error) ||
      !fd.SendTimeout(send_timeout_secs, error) ||
      !fd.ReadTimeout(read_timeout_secs, error) ||
      !fd.ReadLowWatermark(read_min_bytes, error)) {
    return ConnectionPtr();
  }

  return ConnectionImpl::Create(*event_loop_, std::move(fd), end_point);
}

void NetworkServiceImpl::HandleNewConnection(const Passive& fd,
                                             ConnectionPtr connection) {
  auto callback = listen_callbacks_.find(fd.native());
  DCHECK(callback != listen_callbacks_.end());

  if (!connection->SendTimeout(send_timeout_secs) ||
      !connection->ReadTimeout(read_timeout_secs)) {
    callback->second(ConnectionPtr());
  }

  callback->second(connection);
}

}  // namespace net
}  // namespace dist_clang
