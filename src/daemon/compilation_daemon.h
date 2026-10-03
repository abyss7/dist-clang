#pragma once

#include <base/process_forward.h>
#include <cache/file_cache.h>
#include <cache/file_hash_cache.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class CompilationDaemon : public BaseDaemon {
 public:
  bool Initialize() override;

  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        ui32 user_id,
                                        const Path& cwd_path = Path());
  static base::ProcessPtr CreateProcess(const base::proto::Flags& flags,
                                        const Path& cwd_path = Path());

  static cache::string::HandledHash GenerateHash(
      const base::proto::Flags& flags, const cache::string::HandledSource& code,
      const cache::ExtraFiles& extra_files);

  // Replace the source root of the compilation - see |Flags::key_root| - with
  // a placeholder: in any text, or only in the line markers of the
  // preprocessed source.
  static String NormalizePaths(const String& text,
                               const base::proto::Flags& flags);
  static String NormalizeLineMarkers(const String& source,
                                     const base::proto::Flags& flags);

  // Hash of the warning flags - to know, if the cached diagnostics are valid.
  static String WarningsHash(const base::proto::Flags& flags);

  // Removes ANSI escape sequences of colored diagnostics.
  static String StripColors(const String& text);

  // Generates a hash for an entry, that can't be found by the preprocessed
  // source - only through the direct cache.
  static cache::string::HandledHash GenerateUniqueHash(
      const base::proto::Flags& flags);

  // The target of the dependency rule depends on the output path, which isn't
  // a part of the cache key - so it's replaced with a placeholder in the cache.
  static Immutable NormalizeDeps(Immutable deps, const String& target);
  static Immutable RestoreDeps(Immutable deps, const String& target);

 protected:
  explicit CompilationDaemon(const Configuration& conf);

  bool SetupCompiler(base::proto::Flags* flags,
                     net::proto::Status* status) const;

  bool ReadExtraFiles(const base::proto::Flags& flags,
                      const String& current_dir,
                      cache::ExtraFiles* extra_files) const;

  bool SearchSimpleCache(const cache::string::HandledHash& hash,
                         cache::FileCache::Entry* entry) const;

  // On a miss sets |source_edited|, if another version of the same source
  // was compiled with the same flags before.
  bool SearchDirectCache(const base::proto::Flags& flags,
                         const String& current_dir,
                         cache::FileCache::Entry* entry,
                         bool* source_edited = nullptr) const;

  void UpdateSimpleCache(const cache::string::HandledHash& hash,
                         const cache::FileCache::Entry& entry);

  void UpdateDirectCache(const base::proto::Local* message,
                         const cache::string::HandledHash& hash,
                         const cache::ExtraFiles& extra_files,
                         const cache::FileCache::Entry& entry);

  bool Check(const Configuration& conf) const override;
  inline bool Reload(const Configuration& conf) override {
    return BaseDaemon::Reload(conf);
  }

 private:
  using PluginNameMap = HashMap<String /* name */, String /* path */>;

  UniquePtr<cache::FileCache> cache_;
  mutable cache::FileHashCache file_hashes_;
};

}  // namespace daemon
}  // namespace dist_clang
