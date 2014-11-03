#pragma once

#include <base/aliases.h>
#include <base/process_forward.h>
#include <daemon/configuration.pb.h>
#include <file_cache/file_cache.h>
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
                                        const String& cwd_path = String());
  static base::ProcessPtr CreateProcess(const proto::Flags& flags,
                                        const String& cwd_path = String());

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

  inline bool Listen(const String& host, ui32 port, String* error = nullptr) {
    using namespace std::placeholders;
    return network_service_->Listen(
        host, port, std::bind(&BaseDaemon::HandleNewConnection, this, _1),
        error);
  }

  inline auto Connect(net::EndPointPtr end_point, String* error = nullptr) {
    return network_service_->Connect(end_point, error);
  }

  file_cache::string::HandledHash GenerateHash(
      const proto::Flags& flags,
      const file_cache::string::HandledSource& code) const;

  bool SetupCompiler(proto::Flags* flags, proto::Status* status) const;

  bool SearchSimpleCache(const proto::Flags& flags,
                         const file_cache::string::HandledSource& source,
                         FileCache::Entry* entry) const;

  bool SearchDirectCache(const proto::Flags& flags, const String& current_dir,
                         FileCache::Entry* entry) const;

  void UpdateSimpleCache(const proto::Flags& flags,
                         const file_cache::string::HandledSource& source,
                         const FileCache::Entry& entry);

  void UpdateDirectCache(const proto::LocalExecute* message,
                         const file_cache::string::HandledSource& source,
                         const FileCache::Entry& entry);

  const proto::Configuration conf_;
  UniquePtr<net::EndPointResolver> resolver_;

 private:
  using CompilerMap = HashMap<String /* version */, String /* path */>;
  using PluginNameMap = HashMap<String /* name */, String /* path */>;
  using PluginMap = HashMap<String /* version */, PluginNameMap>;

  void HandleNewConnection(net::ConnectionPtr connection);

  UniquePtr<net::NetworkService> network_service_;
  UniquePtr<FileCache> cache_;
  CompilerMap compilers_;
  PluginMap plugins_;
};

}  // namespace daemon
}  // namespace dist_clang
