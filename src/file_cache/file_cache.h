#pragma once

#include <base/const_string.h>
#include <base/thread_pool.h>
#include <file_cache/database_leveldb.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

namespace dist_clang {

namespace file_cache {

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

}  // namespace file_cache

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

  static file_cache::string::HandledHash Hash(
      const file_cache::string::HandledSource& code,
      const file_cache::string::CommandLine& command_line,
      const file_cache::string::Version& version);

  static file_cache::string::UnhandledHash Hash(
      const file_cache::string::UnhandledSource& code,
      const file_cache::string::CommandLine& command_line,
      const file_cache::string::Version& version);

  bool Find(const file_cache::string::HandledSource& code,
            const file_cache::string::CommandLine& command_line,
            const file_cache::string::Version& version, Entry* entry) const;

  bool Find(const file_cache::string::UnhandledSource& code,
            const file_cache::string::CommandLine& command_line,
            const file_cache::string::Version& version, Entry* entry) const;

  Optional Store(const file_cache::string::HandledSource& code,
                 const file_cache::string::CommandLine& command_line,
                 const file_cache::string::Version& version,
                 const Entry& entry);

  Optional Store(const file_cache::string::UnhandledSource& code,
                 const file_cache::string::CommandLine& command_line,
                 const file_cache::string::Version& version,
                 const List<String>& headers,
                 const file_cache::string::HandledHash& hash);

  Optional StoreNow(const file_cache::string::HandledSource& code,
                    const file_cache::string::CommandLine& command_line,
                    const file_cache::string::Version& version,
                    const Entry& entry);

  Optional StoreNow(const file_cache::string::UnhandledSource& code,
                    const file_cache::string::CommandLine& command_line,
                    const file_cache::string::Version& version,
                    const List<String>& headers,
                    const file_cache::string::HandledHash& hash);

 private:
  FRIEND_TEST(file_cache::FileCacheTest, DoubleLocks);
  FRIEND_TEST(file_cache::FileCacheTest, ExceedCacheSize);
  FRIEND_TEST(file_cache::FileCacheTest, ExceedCacheSize_Sync);
  FRIEND_TEST(file_cache::FileCacheTest, LockNonExistentFile);
  FRIEND_TEST(file_cache::FileCacheTest, RemoveEntry);
  FRIEND_TEST(file_cache::FileCacheTest, RestoreEntryWithMissingFile);

  class ReadLock {
   public:
    ReadLock(const FileCache* WEAK_PTR file_cache, const String& path);
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
    WriteLock(const FileCache* WEAK_PTR file_cache, const String& path);
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

  bool FindByHash(const file_cache::string::HandledHash& hash,
                  Entry* entry) const;
  bool RemoveEntry(const String& manifest_path);
  void DoStore(const file_cache::string::HandledHash& hash, const Entry& entry);
  void DoStore(file_cache::string::UnhandledHash orig_hash,
               const List<String>& headers,
               const file_cache::string::HandledHash& hash);
  void Clean();

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;

  const String path_;
  ui64 max_size_;
  std::atomic<ui64> cached_size_;
  bool snappy_;

  UniquePtr<file_cache::Database> database_;
  base::ThreadPool pool_;
};

}  // namespace dist_clang
