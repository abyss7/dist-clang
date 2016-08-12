#pragma once

#include <base/const_string.h>
#include <base/locked_list.h>
#include <base/thread_pool.h>
#include <cache/database_leveldb.h>
#include <cache/database_sqlite.h>
#include <cache/manifest.pb.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

namespace dist_clang {
namespace cache {

FORWARD_TEST(FileCacheTest, DoubleLocks);
FORWARD_TEST(FileCacheTest, ExceedCacheSize);
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

  struct Entry {
    Immutable object;
    Immutable deps;
    Immutable stderr;
  };

  FileCache(const String& path, ui64 size, bool snappy, bool store_index);
  explicit FileCache(const String& path);
  ~FileCache();

  bool Run(ui64 clean_period);
  // |clean_period| is in seconds.

  static string::HandledHash Hash(string::HandledSource code,
                                  const ExtraFiles& extra_files,
                                  string::CommandLine command_line,
                                  string::Version version);

  static string::UnhandledHash Hash(string::UnhandledSource code,
                                    const ExtraFiles& extra_files,
                                    string::CommandLine command_line,
                                    string::Version version);

  bool Find(string::HandledSource code, const ExtraFiles& extra_files,
            string::CommandLine command_line, string::Version version,
            Entry* entry) const;

  bool Find(string::UnhandledSource code, const ExtraFiles& extra_files,
            string::CommandLine command_line, string::Version version,
            const String& current_dir, Entry* entry) const;

  void Store(string::UnhandledSource code, const ExtraFiles& extra_files,
             string::CommandLine command_line, string::Version version,
             const List<String>& headers, const String& current_dir,
             string::HandledHash hash);

  void Store(string::HandledSource code, const ExtraFiles& extra_files,
             string::CommandLine command_line, string::Version version,
             const Entry& entry);

 private:
  FRIEND_TEST(FileCacheTest, DoubleLocks);
  FRIEND_TEST(FileCacheTest, ExceedCacheSize);
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

  inline String SecondPath(string::Hash hash) const {
    DCHECK(hash.str.size() >= 2);
    return path_ + "/" + hash.str[0] + "/" + hash.str[1];
  }

  inline String CommonPath(string::Hash hash) const {
    return SecondPath(hash) + "/" + hash.str.string_copy();
  }

  bool FindByHash(string::HandledHash hash, Entry* entry) const;
  void DoStore(string::HandledHash hash, Entry entry);
  void DoStore(string::UnhandledHash orig_hash, const List<String>& headers,
               const String& current_dir, const string::HandledHash& hash);

  using TimeHashPair = Pair<ui64 /* mtime */, string::Hash>;
  using EntryList = base::LockedList<TimeHashPair>;
  using EntryListDeleter = Fn<void(EntryList* list)>;

  bool Migrate(string::Hash hash, ui32 to_version = kManifestVersion) const;

  bool GetEntrySize(string::Hash hash, ui64* size) const;
  // Sets |0u| if the entry is broken.
  // Returns |true| if the entry is from index.

  bool RemoveEntry(string::Hash hash);
  // Returns |false| only if some part of entry can't be physically removed.

  void Clean(UniquePtr<EntryList> list);

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;

  const String path_;
  bool snappy_, store_index_;
  UniquePtr<LevelDB> database_;
  UniquePtr<SQLite> entries_;

  ui64 max_size_, cache_size_ = {0u};
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
