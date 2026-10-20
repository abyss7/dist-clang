#pragma once

#include <base/attributes.h>
#include <base/types.h>

#include <base/test_forward.h>

namespace dist_clang {

namespace cache {
FORWARD_TEST(FileHashCacheTest, Memoized);
}  // namespace cache

namespace cache {

// Memoizes hashes of file contents. A hash is reused while the file's inode,
// size, mtime and ctime stay the same.
class FileHashCache {
 public:
  // Files modified so recently may still be modified within the same timestamp
  // granularity - their hashes aren't memoized.
  enum : i64 { kRacyIntervalNs = 2'000'000'000 };

  // At most |max_entries| hashes are memoized - the paths of the removed files,
  // of the old checkouts and so on shouldn't grow the daemon forever.
  explicit FileHashCache(size_t max_entries = 1'000'000,
                         i64 racy_interval_ns = kRacyIntervalNs)
      : max_entries_(max_entries), racy_interval_ns_(racy_interval_ns) {}

  // Returns the hex-encoded hash of the contents of |path|. Sets
  // |time_macros|, if the file uses "__DATE__", "__TIME__" or "__TIMESTAMP__":
  // the result of its compilation depends on the time.
  // Sets |modified|, the latest time of the modification of the file (mtime or
  // ctime) in nanoseconds.
  bool Hash(const String& path, String* hash, String* error = nullptr,
            bool* time_macros = nullptr, i64* modified = nullptr) THREAD_SAFE;

  // The number of the memoized hashes.
  size_t size() const THREAD_SAFE;

 private:
  FRIEND_TEST(FileHashCacheTest, Memoized);

  struct Entry {
    ui64 inode, size;
    i64 mtime_ns, ctime_ns;
    String hash;
    bool time_macros;
  };

  const size_t max_entries_;
  const i64 racy_interval_ns_;
  mutable Mutex mutex_;
  HashMap<String, Entry> entries_;
};

}  // namespace cache
}  // namespace dist_clang
