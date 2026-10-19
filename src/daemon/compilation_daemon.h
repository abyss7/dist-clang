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
  void Flush() override;

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

  // The current time of file modifications - in nanoseconds.
  static i64 FileSystemTime();

  // Checks, that the source, the included files from |deps| and other inputs
  // weren't modified since |time|. Otherwise the result of the compilation
  // may not match the inputs, that are read now - and mustn't be cached.
  static bool InputsUnchangedSince(const base::proto::Local* message,
                                   Immutable deps, i64 time);

  // The command line in the key of the direct cache.
  static String DirectCommandLine(const String& current_dir,
                                  const base::proto::Flags& flags);

  // Finds the input or the output, that exists, but isn't a regular file -
  // e.g. "/dev/null": such compilations aren't cached. Sets its |path|.
  static bool FindSpecialFile(const base::proto::Flags& flags,
                              const String& current_dir, String* path);

  // The directories, where the compiler searches the included headers, in the
  // order of the search: the directory of the source, "-iquote", "-I",
  // "-isystem", the implicit directories of the compiler and "-idirafter"
  // ones - as absolute normal paths.
  static List<Path> IncludeSearchDirs(const base::proto::Flags& flags,
                                      const String& current_dir);

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
  // The same for the source root: the entry may be found from another one.
  static Immutable NormalizeDeps(Immutable deps,
                                 const base::proto::Flags& flags);
  static Immutable RestoreDeps(Immutable deps,
                               const base::proto::Flags& flags);

  // The same for the paths of the sources in the trace of the time.
  static Immutable NormalizeTimeTrace(Immutable trace,
                                      const base::proto::Flags& flags);
  static Immutable RestoreTimeTrace(Immutable trace,
                                    const base::proto::Flags& flags);

  // Hash of the options, that change the trace of the time - but not its
  // place.
  static String TimeTraceOptions(const base::proto::Flags& flags);

 protected:
  explicit CompilationDaemon(const Configuration& conf);

  bool SetupCompiler(base::proto::Flags* flags,
                     net::proto::Status* status) const;

  bool ReadExtraFiles(const base::proto::Flags& flags,
                      const String& current_dir,
                      cache::ExtraFiles* extra_files) const;

  bool SearchSimpleCache(const cache::string::HandledHash& hash,
                         cache::FileCache::Entry* entry) const;

  // Reads the trace of the time of the found entry.
  bool SearchTimeTrace(const cache::FileCache::Entry& entry,
                       Immutable* trace) const;

  // On a miss sets |source_edited|, if another version of the same source
  // was compiled with the same flags before, and |source_seen|, if the same
  // source was compiled with the same flags, except the preprocessing-only
  // ones - i.e. the simple cache may have the result.
  bool SearchDirectCache(const base::proto::Flags& flags,
                         const String& current_dir,
                         cache::FileCache::Entry* entry,
                         bool* source_edited = nullptr,
                         bool* source_seen = nullptr) const;

  void UpdateSimpleCache(const cache::string::HandledHash& hash,
                         const cache::FileCache::Entry& entry);

  // Removes the wrong result from the cache.
  void RemoveFromCache(const cache::string::HandledHash& hash);

  // Tags the entry with the tags of the client - see |base::kEnvTags|.
  void TagEntry(const base::proto::Local& message,
                const cache::string::HandledHash& hash);

  // Replies with the statistics of the tags, or removes a tag - in
  // background.
  bool HandleTagsRequest(net::ConnectionPtr connection,
                         const base::proto::TagsRequest& request);

  // |start_time| is the time of the start of the compilation - see
  // |FileSystemTime()|.
  // The |deps| are the ones of the build directory - see |RestoreDeps()|.
  void UpdateDirectCache(const base::proto::Local* message,
                         const cache::string::HandledHash& hash,
                         const cache::ExtraFiles& extra_files, Immutable deps,
                         i64 start_time);

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
