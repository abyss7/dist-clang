#pragma once

#include <base/process_forward.h>
#include <cache/file_cache.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class CompilationDaemon : public BaseDaemon {
 public:
  bool Initialize() override;
  bool UpdateConfCompilersAndPlugins(
      const proto::Configuration& configuration) override;

  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        ui32 user_id,
                                        Immutable cwd_path = Immutable());
  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        Immutable cwd_path = Immutable());

 protected:
  explicit CompilationDaemon(const proto::Configuration& configuration);

  cache::string::HandledHash GenerateHash(
      const base::proto::Flags& flags,
      const cache::string::HandledSource& code) const;

  bool SetupCompiler(base::proto::Flags* flags,
                     net::proto::Status* status) const;

  bool SearchSimpleCache(const base::proto::Flags& flags,
                         const cache::string::HandledSource& source,
                         cache::FileCache::Entry* entry) const;

  bool SearchDirectCache(const base::proto::Flags& flags,
                         const String& current_dir,
                         cache::FileCache::Entry* entry) const;

  void UpdateSimpleCache(const base::proto::Flags& flags,
                         const cache::string::HandledSource& source,
                         const cache::FileCache::Entry& entry);

  void UpdateDirectCache(const base::proto::Local* message,
                         const cache::string::HandledSource& source,
                         const cache::FileCache::Entry& entry);

  //Убираю const, make shared _ptr
  SharedPtr<proto::Configuration> conf_;

 private:
  using CompilerMap = HashMap<String /* version */, String /* path */>;
  using PluginNameMap = HashMap<String /* name */, String /* path */>;
  using PluginMap = HashMap<String /* version */, PluginNameMap>;

  UniquePtr<cache::FileCache> cache_;
  // make shared_ptr
  SharedPtr<CompilerMap> compilers_;
  SharedPtr<PluginMap> plugins_;
};

}  // namespace daemon
}  // namespace dist_clang
