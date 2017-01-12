#pragma once

#include <base/aliases.h>
#include <daemon/configuration.h>
#include <daemon/remote.pb.h>
#include <net/connection_forward.h>
#include <net/end_point_resolver.h>
#include <net/network_service.h>

namespace dist_clang {
namespace daemon {

/** Daemon and configuration.
 *
 *  Construction.
 *      Use configuration only to initialize non-reloadable internals. Don't
 *      call |UpdateConfiguration()| here since it's a virtual method!
 *
 *  Initialization.
 *      |Initialize()| reloadable parts (via |UpdateConfiguration()|,
 *      and parts that may fail since we don't use exceptions. Don't forget to
 *      call the method |Initialize()| of a base class.
 *
 *  Configuration update.
 *      Configuration may be updated in a thread-safe (read, atomic) manner at
 *      any time, and the daemon should reload corresponding parts.
 *      |UpdateConfiguration()| should check the correctness of new
 *      configuration, the full new configuration is required. Don't forget to
 *      call the method |UpdateConfiguration()| of a base class.
 *
 *  Configuration usage.
 *      If some method uses configuration then store the shared pointer at the
 *      beginning (via |conf()|) so that configuration won't suddenly change.
 */
class BaseDaemon {
 public:
  using Configuration = proto::Configuration;
  using ConfigurationPtr = SharedPtr<Configuration>;

  virtual ~BaseDaemon();
  virtual bool Initialize() THREAD_UNSAFE = 0;

  bool Update(const Configuration& conf) THREAD_SAFE;

 protected:
  using Universal = UniquePtr<net::proto::Universal>;

  explicit BaseDaemon(const Configuration& conf);

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

  inline ConfigurationPtr conf() const THREAD_SAFE {
    UniqueLock lock(conf_mutex_);
    return conf_;
  }

  // Check if new configuration is proper. Chain calls of this method from
  // derived to base classes in the beginning, so that more basic checks are
  // done first.
  inline virtual bool Check(const Configuration& conf) const {
    conf.CheckInitialized();
    return true;
  }

  // Reload internals according to new configuration.
  // Should be thread-safe due to the |Update()| thread-safeness.
  inline virtual bool Reload(const Configuration& conf) THREAD_SAFE {
    return true;
  }

  inline bool Check() const { return Check(*conf_); }
  inline bool Reload() { return Reload(*conf_); }

  UniquePtr<net::EndPointResolver> resolver_;

 private:
  void HandleNewConnection(net::ConnectionPtr connection);

  // TODO(ilezhankin): Implement and use WeaklessPtr here. Otherwise, the
  //                   |SharedPtr::reset()| is non-atomic and thread-unsafe
  //                   without mutex.
  mutable Mutex conf_mutex_;
  ConfigurationPtr conf_;

  UniquePtr<net::NetworkService> network_service_;
};

}  // namespace daemon
}  // namespace dist_clang
