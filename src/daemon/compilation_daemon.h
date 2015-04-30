#pragma once

#include <base/process_forward.h>
#include <cache/file_cache.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class CompilationDaemon : public BaseDaemon {
 public:
  bool Initialize() override;

  static base::ProcessPtr CreateProcess(const proto::Flags& flags, ui32 user_id,
                                        Immutable cwd_path = Immutable());
  static base::ProcessPtr CreateProcess(const proto::Flags& flags,
                                        Immutable cwd_path = Immutable());

 protected:
  explicit CompilationDaemon(const proto::Configuration& configuration);

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

 private:
  using CompilerMap = HashMap<String /* version */, String /* path */>;
  using PluginNameMap = HashMap<String /* name */, String /* path */>;
  using PluginMap = HashMap<String /* version */, PluginNameMap>;

  UniquePtr<cache::FileCache> cache_;
  CompilerMap compilers_;
  PluginMap plugins_;
};

}  // namespace daemon
}  // namespace dist_clang
