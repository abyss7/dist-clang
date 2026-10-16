#include <cache/file_hash_cache.h>

#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/string_utils.h>

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

bool FileHashCache::Hash(const Path& path, String* hash, String* error,
                         bool* time_macros, i64* modified) {
  struct stat st;
  if (stat(path.c_str(), &st) == -1) {
    base::GetLastError(error);
    return false;
  }

  if (modified) {
    *modified = std::max(ToNs(st.st_mtim), ToNs(st.st_ctim));
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
      if (time_macros) {
        *time_macros = it->second.time_macros;
      }
      return true;
    }
  }

  Immutable content;
  if (!base::File::Read(path, &content, error)) {
    return false;
  }
  // The contents must match the time of the modification.
  struct stat new_st;
  if (stat(path.c_str(), &new_st) == -1 || new_st.st_ino != st.st_ino ||
      new_st.st_size != st.st_size ||
      ToNs(new_st.st_mtim) != ToNs(st.st_mtim) ||
      ToNs(new_st.st_ctim) != ToNs(st.st_ctim)) {
    if (error) {
      error->assign("modified while hashing");
    }
    return false;
  }
  *hash = base::Hexify(content.Hash());
  bool has_time_macros = false;
  for (const char* macro : {"__DATE__", "__TIME__", "__TIMESTAMP__"}) {
    has_time_macros = has_time_macros || content.find(macro) != String::npos;
  }
  if (time_macros) {
    *time_macros = has_time_macros;
  }

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (ToNs(now) - ToNs(st.st_mtim) > kRacyIntervalNs &&
      ToNs(now) - ToNs(st.st_ctim) > kRacyIntervalNs) {
    std::lock_guard<Mutex> lock(mutex_);
    entries_[key] = {static_cast<ui64>(st.st_ino),
                     static_cast<ui64>(st.st_size), ToNs(st.st_mtim),
                     ToNs(st.st_ctim), *hash, has_time_macros};
  }

  return true;
}

}  // namespace cache
}  // namespace dist_clang
