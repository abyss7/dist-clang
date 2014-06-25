#pragma once

#include <base/thread_pool.h>

#include <third_party/gtest/public/gtest/gtest_prod.h>

namespace dist_clang {

namespace file_cache {
FORWARD_TEST(FileCacheTest, LockNonExistentFile);
FORWARD_TEST(FileCacheTest, DoubleLocks);
FORWARD_TEST(FileCacheTest, RemoveEntry);
}  // namespace file_cache

class FileCache {
 public:
  enum : ui64 {
    UNLIMITED = 0,
  };

  struct Entry {
    String object_path;
    String deps_path;
    String stderr;
    bool move_object = false;
    bool move_deps = false;
  };

  using Optional = base::ThreadPool::Optional;

  FileCache(const String& path, ui64 size);
  explicit FileCache(const String& path);

  static String Hash(const String& code, const String& command_line,
                     const String& version);
  bool Find(const String& code, const String& command_line,
            const String& version, Entry* entry) const;
  Optional Store(const String& code, const String& command_line,
                 const String& version, const Entry& entry);
  void StoreNow(const String& code, const String& command_line,
                const String& version, const Entry& entry);

 private:
  FRIEND_TEST(file_cache::FileCacheTest, LockNonExistentFile);
  FRIEND_TEST(file_cache::FileCacheTest, DoubleLocks);
  FRIEND_TEST(file_cache::FileCacheTest, RemoveEntry);

  inline String FirstPath(const String& hash) const {
    return path_ + "/" + hash[0];
  }
  inline String SecondPath(const String& hash) const {
    return FirstPath(hash) + "/" + hash[1];
  }
  inline String CommonPath(const String& hash) const {
    return SecondPath(hash) + "/" + hash.substr(2);
  }

  bool RemoveEntry(const String& manifest_path);
  void DoStore(const String& hash, const Entry& entry);
  void Clean();

  // FIXME: convert those methods to RAII.
  bool LockForReading(const String& path) const;
  void UnlockForReading(const String& path) const;
  bool LockForWriting(const String& path) const;
  void UnlockForWriting(const String& path) const;

  const String path_;
  base::ThreadPool pool_;
  ui64 max_size_;
  std::atomic<ui64> cached_size_;

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;
};

}  // namespace dist_clang
