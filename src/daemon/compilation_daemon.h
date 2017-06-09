#pragma once

#include <base/process_forward.h>
#include <cache/file_cache.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class CompilationDaemon : public BaseDaemon {
 public:
  bool Initialize() override;

  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        ui32 user_id,
                                        Immutable cwd_path = Immutable());
  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        Immutable cwd_path = Immutable());

 protected:
  explicit CompilationDaemon(const Configuration& conf);

  cache::string::HandledHash GenerateHash(
      const base::proto::Flags& flags, const cache::string::HandledSource& code,
      const cache::ExtraFiles& extra_files) const;

  bool SetupCompiler(base::proto::Flags* flags,
                     net::proto::Status* status) const;

  bool ReadExtraFiles(const base::proto::Flags& flags,
                      const String& current_dir,
                      cache::ExtraFiles* extra_files) const;

  bool SearchSimpleCache(const cache::string::HandledHash& hash,
                         cache::FileCache::Entry* entry) const;

  bool SearchDirectCache(const base::proto::Flags& flags,
                         const String& current_dir,
                         cache::FileCache::Entry* entry) const;

  void UpdateSimpleCache(const cache::string::HandledHash& hash,
                         const cache::FileCache::Entry& entry);

  void UpdateDirectCache(const base::proto::Local* message,
                         const cache::string::HandledSource& source,
                         const cache::ExtraFiles& extra_files,
                         const cache::FileCache::Entry& entry);

  bool Check(const Configuration& conf) const override;
  inline bool Reload(const Configuration& conf) override {
    return BaseDaemon::Reload(conf);
  }

 private:
  using PluginNameMap = HashMap<String /* name */, String /* path */>;

  UniquePtr<cache::FileCache> cache_;
};

}  // namespace daemon
}  // namespace dist_clang
