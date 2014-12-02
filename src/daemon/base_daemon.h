#pragma once

#include <base/aliases.h>
#include <base/process_forward.h>
#include <cache/file_cache.h>
#include <daemon/configuration.pb.h>
#include <net/connection_forward.h>
#include <net/end_point_resolver.h>
#include <net/network_service.h>
#include <proto/remote.pb.h>

namespace dist_clang {
namespace daemon {

class BaseDaemon {
 public:
  virtual ~BaseDaemon();
  virtual bool Initialize() = 0;

  static base::ProcessPtr CreateProcess(const proto::Flags& flags, ui32 user_id,
                                        Immutable cwd_path = Immutable());
  static base::ProcessPtr CreateProcess(const proto::Flags& flags,
                                        Immutable cwd_path = Immutable());

 protected:
  using Universal = UniquePtr<proto::Universal>;

  BaseDaemon(const proto::Configuration& configuration);

  virtual bool HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const proto::Status& status) = 0;

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

  cache::string::HandledHash GenerateHash(
      const proto::Flags& flags,
      const cache::string::HandledSource& code) const;

  bool SetupCompiler(proto::Flags* flags, proto::Status* status) const;

  bool SearchSimpleCache(const proto::Flags& flags,
                         const cache::string::HandledSource& source,
                         cache::FileCache::Entry* entry) const;

  bool SearchDirectCache(const proto::Flags& flags, const String& current_dir,
                         cache::FileCache::Entry* entry) const;

  void UpdateSimpleCache(const proto::Flags& flags,
                         const cache::string::HandledSource& source,
                         const cache::FileCache::Entry& entry);

  void UpdateDirectCache(const proto::LocalExecute* message,
                         const cache::string::HandledSource& source,
                         const cache::FileCache::Entry& entry);

  const proto::Configuration conf_;
  UniquePtr<net::EndPointResolver> resolver_;

 private:
  using CompilerMap = HashMap<String /* version */, String /* path */>;
  using PluginNameMap = HashMap<String /* name */, String /* path */>;
  using PluginMap = HashMap<String /* version */, PluginNameMap>;

  void HandleNewConnection(net::ConnectionPtr connection);

  UniquePtr<net::NetworkService> network_service_;
  UniquePtr<cache::FileCache> cache_;
  CompilerMap compilers_;
  PluginMap plugins_;
};

}  // namespace daemon
}  // namespace dist_clang
