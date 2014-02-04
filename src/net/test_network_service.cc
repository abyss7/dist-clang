#include "net/test_network_service.h"

namespace dist_clang {
namespace net {

std::unique_ptr<NetworkService> TestNetworkService::Factory::Create() {
  auto new_t = new TestNetworkService;
  on_create_(new_t);
  return std::unique_ptr<NetworkService>(new_t);
}

bool TestNetworkService::Listen(
    const std::string& path,
    ListenCallback callback,
    std::string* error) {
  if (listen_attempts_) {
    (*listen_attempts_)++;
  }

  listen_callbacks_[std::make_pair(path, 0)] = callback;

  return on_listen_(path, 0, error);
}

bool TestNetworkService::Listen(
    const std::string& host,
    unsigned short port,
    ListenCallback callback,
    std::string* error) {
  if (listen_attempts_) {
    (*listen_attempts_)++;
  }

  listen_callbacks_[std::make_pair(host, port)] = callback;

  return on_listen_(host, port, error);
}

ConnectionPtr TestNetworkService::Connect(
    EndPointPtr end_point,
    std::string* error) {
  if (connect_attempts_) {
    (*connect_attempts_)++;
  }

  return on_connect_(end_point, error);
}

ConnectionPtr TestNetworkService::TriggerListen(
    const std::string& host,
    uint16_t port) {
  auto it = listen_callbacks_.find(std::make_pair(host, port));
  if (it != listen_callbacks_.end()) {
    if (connect_attempts_) {
      (*connect_attempts_)++;
    }
    auto new_connection =
        on_connect_(EndPoint::TcpHost(host, port), nullptr);
    it->second(new_connection);
    return new_connection;
  }
  return ConnectionPtr();
}

}  // namespace net
}  // namespace dist_clang
