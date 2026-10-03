#pragma once

#include <base/const_string.h>
#include <base/locked_list.h>
#include <base/thread_pool.h>
#include <cache/database_kv.h>
#include <cache/database_sqlite.h>
#include <cache/file_hash_cache.h>
#include <cache/manifest.pb.h>

#include <base/test_forward.h>

namespace dist_clang {
namespace cache {

FORWARD_TEST(FileCacheTest, DoubleLocks);
FORWARD_TEST(FileCacheTest, ExceedCacheSize);
FORWARD_TEST(FileCacheTest, EvictNeverHitEntriesFirst);
FORWARD_TEST(FileCacheTest, ExceedCacheSize_Sync);
FORWARD_TEST(FileCacheTest, LockNonExistentFile);
FORWARD_TEST(FileCacheTest, RemoveEntry);
FORWARD_TEST(FileCacheTest, RestoreEntryWithMissingFile);
FORWARD_TEST(FileCacheTest, UseIndexFromDisk);
FORWARD_TEST(FileCacheMigratorTest, Version_0_to_1_Direct);
FORWARD_TEST(FileCacheMigratorTest, Version_0_to_1_Simple);
FORWARD_TEST(FileCacheMigratorTest, Version_1_to_2_Direct);
FORWARD_TEST(FileCacheMigratorTest, Version_1_to_2_Simple);

enum ExtraFileType {
  SANITIZE_BLACKLIST = 0,
  KEY_FILES = 1,
  // Hashes of files, which contents affect the result, e.g. profiles.
};

using ExtraFiles = HashMap<ExtraFileType, Immutable>;

namespace string {

#define DEFINE_STRING_TYPE(type)                                          \
  struct type {                                                           \
    type() = default;                                                     \
    explicit type(Immutable str) : str(str) {}                            \
    operator Immutable() const { return str; }                            \
    bool operator==(const type& other) const { return str == other.str; } \
                                                                          \
    /* TODO: remove this */                                               \
    explicit type(const String& str) : str(str) {}                        \
                                                                          \
    Immutable str;                                                        \
  }

#define DEFINE_STRING_SUBTYPE(type, base_type)           \
  struct type : public base_type {                       \
    type() = default;                                    \
    explicit type(Immutable str) : base_type(str) {}     \
                                                         \
    /* TODO: remove this */                              \
    explicit type(const String& str) : base_type(str) {} \
  }

DEFINE_STRING_TYPE(HandledSource);
DEFINE_STRING_TYPE(UnhandledSource);
DEFINE_STRING_TYPE(Hash);
DEFINE_STRING_SUBTYPE(HandledHash, Hash);
DEFINE_STRING_SUBTYPE(UnhandledHash, Hash);
DEFINE_STRING_TYPE(CommandLine);
DEFINE_STRING_TYPE(Version);

#undef DEFINE_STRING_SUBTYPE
#undef DEFINE_STRING_TYPE

}  // namespace string
}  // namespace cache
}  // namespace dist_clang

namespace std {

template <>
struct hash<dist_clang::cache::string::Hash> {
 public:
  size_t operator()(dist_clang::cache::string::Hash value) const {
    return std::hash<dist_clang::Immutable>()(value.str);
  }
};

}  // namespace std

namespace dist_clang {
namespace cache {

class FileCache {
 public:
  enum : ui64 {
    UNLIMITED = 0,
  };

  // Maximal number of the header sets per one source in the direct cache.
  enum : size_t { kMaxDirectVariants = 8 };

  struct Entry {
    Immutable object;
    Immutable deps;
    Immutable stderr;
    Immutable dwo;

    // With linked outputs the object and split DWARF files are passed by
    // paths instead of contents: the produced files for |Store()| and the
    // files in the cache from |Find()|.
    Path object_path;
    Path dwo_path;

    // Hashes of the linked files from |Find()|.
    String object_hash;
    String dwo_hash;

    // Hash of the warning flags of the compilation.
    String warnings_hash;
  };

  // With |link_outputs| the outputs are stored uncompressed, and are linked
  // into and out of the cache without copying, where possible.
  FileCache(const Path& path, ui64 size, bool snappy, bool store_index,
            bool link_outputs = false);
  explicit FileCache(const Path& path);
  ~FileCache();

  bool Run(ui64 clean_period);
  // |clean_period| is in seconds.

  // Removes entries, while the disk has less free space - besides the size
  // limit. Only the entries, that aren't linked with build directories, count.
  inline void SetMinFreePercent(ui32 percent) { min_free_percent_ = percent; }

  static string::HandledHash Hash(string::HandledSource code,
                                  const ExtraFiles& extra_files,
                                  string::CommandLine command_line,
                                  string::Version version);

  static string::UnhandledHash Hash(string::UnhandledSource code,
                                    const ExtraFiles& extra_files,
                                    string::CommandLine command_line,
                                    string::Version version);

  bool Find(string::UnhandledSource code, const ExtraFiles& extra_files,
            string::CommandLine command_line, string::Version version,
            const Path& current_dir, Entry* entry) const;

  bool Find(string::HandledHash hash, Entry* entry) const;

  void Store(string::UnhandledSource code, const ExtraFiles& extra_files,
             string::CommandLine command_line, string::Version version,
             const List<String>& headers,
             const List<String>& preprocessed_headers, const Path& current_dir,
             string::HandledHash hash);

  void Store(string::HandledHash hash, Entry entry);

  // Remembers the last version of the source, compiled with the same flags,
  // under |key|. Returns |true| if the previous version is different - i.e.
  // the source is edited.
  bool ReplaceLastSource(const String& key, string::UnhandledHash hash);

 private:
  FRIEND_TEST(FileCacheTest, DoubleLocks);
  FRIEND_TEST(FileCacheTest, ExceedCacheSize);
  FRIEND_TEST(FileCacheTest, EvictNeverHitEntriesFirst);
  FRIEND_TEST(FileCacheTest, LockNonExistentFile);
  FRIEND_TEST(FileCacheTest, RemoveEntry);
  FRIEND_TEST(FileCacheTest, RestoreEntryWithMissingFile);
  FRIEND_TEST(FileCacheTest, UseIndexFromDisk);
  FRIEND_TEST(FileCacheMigratorTest, Version_0_to_1_Direct);
  FRIEND_TEST(FileCacheMigratorTest, Version_0_to_1_Simple);
  FRIEND_TEST(FileCacheMigratorTest, Version_1_to_2_Direct);
  FRIEND_TEST(FileCacheMigratorTest, Version_1_to_2_Simple);

  enum : ui32 { kManifestVersion = 2 };

  class ReadLock {
   public:
    ReadLock(const FileCache* WEAK_PTR cache, const String& path);
    ~ReadLock() { Unlock(); }

    inline operator bool() const { return locked_; }
    void Unlock();

   private:
    const FileCache* cache_;
    const String path_;
    bool locked_ = false;
  };

  class WriteLock {
   public:
    WriteLock(const FileCache* WEAK_PTR cache, const String& path);
    ~WriteLock() { Unlock(); }

    inline operator bool() const { return locked_; }
    void Unlock();

   private:
    const FileCache* cache_;
    const String path_;
    bool locked_ = false;
  };

  friend class ReadLock;
  friend class WriteLock;

  inline Path SecondPath(string::Hash hash) const {
    DCHECK(hash.str.size() >= 2);
    return path_ / String(1, hash.str[0]) / String(1, hash.str[1]);
  }

  inline Path CommonPath(string::Hash hash) const {
    return SecondPath(hash) / hash.str.string_copy();
  }

  void DoStore(string::UnhandledHash orig_hash, const List<String>& headers,
               const List<String>& preprocessed_headers,
               const Path& current_dir, const string::HandledHash& hash);

  struct Access {
    ui64 time;
    string::Hash hash;
    bool hit;  // |false| if the entry is just stored.
  };
  using EntryList = base::LockedList<Access>;
  using EntryListDeleter = Fn<void(EntryList* list)>;

  bool Migrate(string::Hash hash, ui32 to_version = kManifestVersion) const;

  bool FindOutput(const Path& cache_path, bool compressed,
                  const String& expected_hash, Immutable* content, Path* path,
                  ui64* size) const;
  bool StoreOutput(const Path& cache_path, Immutable content, const Path& path,
                   String* hash, ui64* size, String* error);

  bool GetEntrySize(string::Hash hash, ui64* size) const;
  // Sets |0u| if the entry is broken.
  // Returns |true| if the entry is from index.

  bool RemoveEntry(string::Hash hash);
  // Returns |false| only if some part of entry can't be physically removed.

  void Clean(UniquePtr<EntryList> list);

  // Removes entries, if the cache is too large, or the disk is too full.
  void Evict();

  // Size of the outputs of the entry, that are hard-linked with other places.
  ui64 GetSharedSize(string::Hash hash) const;

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;

  const Path path_;
  bool snappy_, store_index_, link_outputs_;
  UniquePtr<SQLiteKV> database_;
  mutable FileHashCache file_hashes_;
  UniquePtr<SQLite> entries_;

  ui64 max_size_, cache_size_ = {0u};
  ui32 min_free_percent_ = 0;
  const EntryListDeleter new_entries_deleter_ = [this](EntryList* list) {
    auto task = [this, list] { Clean(UniquePtr<EntryList>(list)); };
    cleaner_.Push(task);
  };
  SharedPtr<EntryList> new_entries_;

  base::ThreadPool cleaner_{base::ThreadPool::TaskQueue::UNLIMITED, 1};

  UniquePtr<base::WorkerPool> resetter_{new base::WorkerPool(true)};
  // Simply resets |new_entries_| periodically.
};

}  // namespace cache
}  // namespace dist_clang
