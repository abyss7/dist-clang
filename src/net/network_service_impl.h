#pragma once

#include <net/event_loop.h>
#include <net/network_service.h>

namespace dist_clang {
namespace net {

class NetworkServiceImpl : public NetworkService {
 public:
  // We need the method |Run()| to allow user to add all listening sockets in
  // a non-threadsafe way, thus, prevent locking inside |HandleNewConnection|.
  virtual bool Run() THREAD_UNSAFE override;

  virtual bool Listen(const String& path, ListenCallback callback,
                      String* error) THREAD_UNSAFE override;
  virtual bool Listen(const String& host, ui16 port, ListenCallback callback,
                      String* error) THREAD_UNSAFE override;

  virtual ConnectionPtr Connect(EndPointPtr end_point,
                                String* error) THREAD_SAFE override;

 private:
  friend class DefaultFactory;

  // FIXME: make these values configurable.
  enum : int {
    read_timeout_secs = 60,
    send_timeout_secs = 5,
    read_min_bytes = 32
  };

  NetworkServiceImpl();

  // |fd| is a descriptor of a listening socket, which accepts new connection.
  void HandleNewConnection(FileDescriptor fd, ConnectionPtr connection);

  UniquePtr<EventLoop> event_loop_;
  HashMap<FileDescriptor, ListenCallback> listen_callbacks_;
};

}  // namespace net
}  // namespace dist_clang
