#pragma once

#include "base/attributes.h"
#include "net/connection_forward.h"
#include "proto/config.pb.h"

#include <atomic>
#include <unordered_set>

namespace std {

template<>
struct hash<dist_clang::proto::Host> {
  size_t operator() (const dist_clang::proto::Host& host) const {
    return hash<string>()(host.host()) * 29 +
           hash<unsigned short>()(host.port());
  }
};

template<>
struct equal_to<dist_clang::proto::Host> {
  bool operator() (const dist_clang::proto::Host& host1,
                   const dist_clang::proto::Host& host2) const {
    if (host1.has_host() != host2.has_host())
      return false;
    if (host1.host() != host2.host())
      return false;
    if (host1.has_port() != host2.has_port())
      return false;
    if (host1.port() != host2.port())
      return false;
    return true;
  }
};

}  // namespace std

namespace dist_clang {

namespace net {
class NetworkService;
}

namespace daemon {

class Balancer {
  public:
    using ConnectCallback =
        std::function<void(net::ConnectionPtr, const std::string&)>;

    explicit Balancer(net::NetworkService& network_service);

    void AddRemote(const proto::Host& remote) THREAD_UNSAFE;
    bool Decide(const ConnectCallback& callback, std::string* error = nullptr);

  private:
    static std::atomic<size_t> index_;

    net::NetworkService& service_;
    std::unordered_set<proto::Host> remotes_;
};

}  // namespace daemon
}  // namespace dist_clang
