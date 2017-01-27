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
 *      Initialize unconditional non-reloadable parts. Make vital checks.
 *      |BaseDaemon| stores initial configuration internally.
 *      Don't call |conf()|, |Check()|, |Reload()| or |Update()| here!
 *
 *  Initialize()
 *      Initialize non-reloadable parts that may fail at run-time.
 *      |BaseDaemon::Initialize()| also calls |Check()| and |Reload()| on
 *      current configuration, thus, initializing reloadable parts too.
 *      Don't forget to call |Initialize()| of a base class at the end!
 *
 *  Check()
 *      Make checks for a _full_ current or new configuration.
 *      This method is called automatically on a configuration update attempt.
 *      Don't forget to call |Check()| of a base class at the beginning!
 *
 *  Reload()
 *      Replace reloadable parts according to a _full_ current or new
 *      configuration, leaving room to restore previous state in case of
 *      failure - new configuration won't be applied if reloading fails.
 *      Don't forget to call |Reload()| of a base class at the end!
 *
 *  Update()
 *      It's a thread-safe (read: atomic) way to update configuration with all
 *      checks and reloads. This method applies new configuration if there are
 *      no any failures.
 *
 *  Configuration usage.
 *      If some method wants to use current configuration, then it should store
 *      the reference at the beginning:
 *
 *          auto conf = this->conf();
 *
 *      It's necessary to make sure that configuration is the same during method
 *      execution. Also it's a little syntax hack that prevents accidental usage
 *      of |conf()| - a compiler will require explicit call |this->conf()|.
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
