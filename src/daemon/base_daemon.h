#pragma once

#include <base/aliases.h>
#include <daemon/configuration.h>
#include <daemon/remote.pb.h>
#include <net/connection_forward.h>
#include <net/end_point_resolver.h>
#include <net/network_service.h>

namespace dist_clang {
namespace daemon {

class BaseDaemon {
 public:
  virtual ~BaseDaemon();
  virtual bool Initialize() = 0;
  virtual bool UpdateConfCompilersAndPlugins(
      const proto::Configuration& configuration) {
    return true;
  }

 protected:
  using Universal = UniquePtr<net::proto::Universal>;

  explicit BaseDaemon(const proto::Configuration& configuration);

  virtual bool HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const net::proto::Status& status) = 0;

  inline bool Listen(const String& path, String* error = nullptr) {
    using namespace std::placeholders;
    return network_service_->Listen(
        path, std::bind(&BaseDaemon::HandleNewConnection, this, _1), error);
  }

  inline bool Listen(const String& host, ui32 port, bool ipv6,
                     String* error = nullptr) {
    using namespace std::placeholders;
    return network_service_->Listen(
        host, port, ipv6, std::bind(&BaseDaemon::HandleNewConnection, this, _1),
        error);
  }

  inline auto Connect(net::EndPointPtr end_point, String* error = nullptr) {
    return network_service_->Connect(end_point, error);
  }

  UniquePtr<net::EndPointResolver> resolver_;

 private:
  void HandleNewConnection(net::ConnectionPtr connection);

  UniquePtr<net::NetworkService> network_service_;
};

}  // namespace daemon
}  // namespace dist_clang
