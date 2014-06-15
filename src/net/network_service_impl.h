#pragma once

#include <net/event_loop.h>
#include <net/network_service.h>

#include <unordered_map>

namespace dist_clang {
namespace net {

class NetworkServiceImpl : public NetworkService {
 public:
  // We need the method |Run()| to allow user to add all listening sockets in
  // a non-threadsafe way, thus, prevent locking inside |HandleNewConnection|.
  virtual bool Run() THREAD_UNSAFE override;

  virtual bool Listen(const std::string& path, ListenCallback callback,
                      std::string* error) THREAD_UNSAFE override;
  virtual bool Listen(const std::string& host, ui16 port,
                      ListenCallback callback,
                      std::string* error) THREAD_UNSAFE override;

  virtual ConnectionPtr Connect(EndPointPtr end_point,
                                std::string* error) THREAD_SAFE override;

 private:
  friend class DefaultFactory;

  NetworkServiceImpl();

  // |fd| is a descriptor of a listening socket, which accepts new connection.
  void HandleNewConnection(fd_t fd, ConnectionPtr connection);

  UniquePtr<EventLoop> event_loop_;
  std::unordered_map<fd_t, ListenCallback> listen_callbacks_;

  // FIXME: make this value configurable.
  const int send_timeout_secs = 5;
};

}  // namespace net
}  // namespace dist_clang
