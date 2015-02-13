#include <net/test_network_service.h>

#include <net/test_connection.h>

namespace dist_clang {
namespace net {

UniquePtr<NetworkService> TestNetworkService::Factory::Create(
    ui32 read_timeout_secs, ui32 send_timeout_secs, ui32 read_min_bytes) {
  auto new_t = new TestNetworkService;
  on_create_(new_t);
  return UniquePtr<NetworkService>(new_t);
}

bool TestNetworkService::Listen(const String& path, ListenCallback callback,
                                String* error) {
  if (listen_attempts_) {
    (*listen_attempts_)++;
  }

  listen_callbacks_[std::make_pair(path, 0)] = callback;

  return on_listen_(path, 0, error);
}

bool TestNetworkService::Listen(const String& host, ui16 port, bool,
                                ListenCallback callback, String* error) {
  if (listen_attempts_) {
    (*listen_attempts_)++;
  }

  listen_callbacks_[std::make_pair(host, port)] = callback;

  return on_listen_(host, port, error);
}

ConnectionPtr TestNetworkService::Connect(EndPointPtr end_point,
                                          String* error) {
  if (connect_attempts_) {
    (*connect_attempts_)++;
  }

  return on_connect_(end_point, error);
}

ConnectionPtr TestNetworkService::TriggerListen(const String& host, ui16 port) {
  auto it = listen_callbacks_.find(std::make_pair(host, port));
  if (it != listen_callbacks_.end()) {
    if (connect_attempts_) {
      (*connect_attempts_)++;
    }
    auto new_connection = on_connect_(EndPointPtr(), nullptr);
    it->second(new_connection);
    return new_connection;
  }
  return ConnectionPtr();
}

}  // namespace net
}  // namespace dist_clang
