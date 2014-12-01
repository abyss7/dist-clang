#pragma once

#include <base/const_string.h>
#include <base/thread_pool.h>
#include <cache/database_leveldb.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

namespace dist_clang {

namespace cache {

FORWARD_TEST(FileCacheTest, DoubleLocks);
FORWARD_TEST(FileCacheTest, ExceedCacheSize);
FORWARD_TEST(FileCacheTest, ExceedCacheSize_Sync);
FORWARD_TEST(FileCacheTest, LockNonExistentFile);
FORWARD_TEST(FileCacheTest, RemoveEntry);
FORWARD_TEST(FileCacheTest, RestoreEntryWithMissingFile);

namespace string {

#define DEFINE_STRING_TYPE(type)                   \
  struct type {                                    \
    type() = default;                              \
    explicit type(Immutable str) : str(str) {}     \
    operator Immutable() const { return str; }     \
                                                   \
    /* TODO: remove this */                        \
    explicit type(const String& str) : str(str) {} \
                                                   \
    Immutable str;                                 \
  }

DEFINE_STRING_TYPE(HandledSource);
DEFINE_STRING_TYPE(UnhandledSource);
DEFINE_STRING_TYPE(HandledHash);
DEFINE_STRING_TYPE(UnhandledHash);
DEFINE_STRING_TYPE(CommandLine);
DEFINE_STRING_TYPE(Version);

#undef DEFINE_STRING_TYPE

}  // namespace string

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

  using Optional = base::ThreadPool::Optional;

  FileCache(const String& path, ui64 size, bool snappy);
  explicit FileCache(const String& path);

  bool Run();

  static cache::string::HandledHash Hash(
      cache::string::HandledSource code,
      cache::string::CommandLine command_line,
      cache::string::Version version);

  static cache::string::UnhandledHash Hash(
      cache::string::UnhandledSource code,
      cache::string::CommandLine command_line,
      cache::string::Version version);

  bool Find(const cache::string::HandledSource& code,
            const cache::string::CommandLine& command_line,
            const cache::string::Version& version, Entry* entry) const;

  bool Find(const cache::string::UnhandledSource& code,
            const cache::string::CommandLine& command_line,
            const cache::string::Version& version, Entry* entry) const;

  Optional Store(const cache::string::HandledSource& code,
                 const cache::string::CommandLine& command_line,
                 const cache::string::Version& version,
                 const Entry& entry);

  Optional Store(const cache::string::UnhandledSource& code,
                 const cache::string::CommandLine& command_line,
                 const cache::string::Version& version,
                 const List<String>& headers,
                 const cache::string::HandledHash& hash);

  Optional StoreNow(const cache::string::HandledSource& code,
                    const cache::string::CommandLine& command_line,
                    const cache::string::Version& version,
                    const Entry& entry);

  Optional StoreNow(const cache::string::UnhandledSource& code,
                    const cache::string::CommandLine& command_line,
                    const cache::string::Version& version,
                    const List<String>& headers,
                    const cache::string::HandledHash& hash);

 private:
  FRIEND_TEST(cache::FileCacheTest, DoubleLocks);
  FRIEND_TEST(cache::FileCacheTest, ExceedCacheSize);
  FRIEND_TEST(cache::FileCacheTest, ExceedCacheSize_Sync);
  FRIEND_TEST(cache::FileCacheTest, LockNonExistentFile);
  FRIEND_TEST(cache::FileCacheTest, RemoveEntry);
  FRIEND_TEST(cache::FileCacheTest, RestoreEntryWithMissingFile);

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

  inline String FirstPath(Immutable hash) const {
    DCHECK(hash.size() > 0);
    return path_ + "/" + hash[0];
  }
  inline String SecondPath(Immutable hash) const {
    DCHECK(hash.size() > 1);
    return FirstPath(hash) + "/" + hash[1];
  }
  inline String CommonPath(Immutable hash) const {
    DCHECK(hash.size() > 2);
    return SecondPath(hash) + "/" + hash.string_copy().substr(2);
  }

  bool FindByHash(const cache::string::HandledHash& hash,
                  Entry* entry) const;
  bool RemoveEntry(const String& manifest_path);
  void DoStore(const cache::string::HandledHash& hash, const Entry& entry);
  void DoStore(cache::string::UnhandledHash orig_hash,
               const List<String>& headers,
               const cache::string::HandledHash& hash);
  void Clean();

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;

  const String path_;
  ui64 max_size_;
  std::atomic<ui64> cached_size_;
  bool snappy_;

  UniquePtr<cache::Database> database_;
  base::ThreadPool pool_;
};

}  // namespace cache
}  // namespace dist_clang
