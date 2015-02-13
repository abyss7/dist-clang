#pragma once

#include <net/event_loop.h>
#include <net/passive.h>
#include <net/network_service.h>

namespace dist_clang {
namespace net {

class NetworkServiceImpl : public NetworkService {
 public:
  virtual ~NetworkServiceImpl();

  // We need the method |Run()| to allow user to add all listening sockets in
  // a non-threadsafe way, thus, prevent locking inside |HandleNewConnection|.
  bool Run() THREAD_UNSAFE override;

  bool Listen(const String& path, ListenCallback callback,
              String* error) THREAD_UNSAFE override;
  bool Listen(const String& host, ui16 port, bool ipv6, ListenCallback callback,
              String* error) THREAD_UNSAFE override;

  virtual ConnectionPtr Connect(EndPointPtr end_point,
                                String* error) THREAD_SAFE override;

 private:
  friend class DefaultFactory;

  NetworkServiceImpl(ui32 read_timeout_secs, ui32 send_timeout_secs,
                     ui32 read_min_bytes);

  void HandleNewConnection(const Passive& fd, ConnectionPtr connection);

  UniquePtr<EventLoop> event_loop_;

  // FIXME: implement true |Passive::Ref|.
  HashMap<Passive::NativeType, ListenCallback> listen_callbacks_;

  const ui32 read_timeout_secs_, send_timeout_secs_, read_min_bytes_;
  List<String> unix_sockets_;
};

}  // namespace net
}  // namespace dist_clang
