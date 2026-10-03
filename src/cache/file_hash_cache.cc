#include <cache/file_hash_cache.h>

#include <base/c_utils.h>
#include <base/file/file.h>

#include <sys/stat.h>
#include <time.h>

namespace dist_clang {
namespace cache {

namespace {

// Files modified so recently may still be modified within the same timestamp
// granularity - don't memoize their hashes.
const i64 kRacyIntervalNs = 2'000'000'000;

i64 ToNs(const struct timespec& time) {
  return static_cast<i64>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

}  // namespace

bool FileHashCache::Hash(const Path& path, String* hash, String* error) {
  struct stat st;
  if (stat(path.c_str(), &st) == -1) {
    base::GetLastError(error);
    return false;
  }

  const String key = path.string();
  {
    std::lock_guard<Mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.inode == st.st_ino &&
        it->second.size == static_cast<ui64>(st.st_size) &&
        it->second.mtime_ns == ToNs(st.st_mtim) &&
        it->second.ctime_ns == ToNs(st.st_ctim)) {
      *hash = it->second.hash;
      return true;
    }
  }

  Immutable content_hash;
  if (!base::File::Hash(path, &content_hash, {}, error)) {
    return false;
  }
  *hash = content_hash.string_copy();

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (ToNs(now) - ToNs(st.st_mtim) > kRacyIntervalNs &&
      ToNs(now) - ToNs(st.st_ctim) > kRacyIntervalNs) {
    std::lock_guard<Mutex> lock(mutex_);
    entries_[key] = {static_cast<ui64>(st.st_ino),
                     static_cast<ui64>(st.st_size), ToNs(st.st_mtim),
                     ToNs(st.st_ctim), *hash};
  }

  return true;
}

}  // namespace cache
}  // namespace dist_clang
