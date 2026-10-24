#include <cache/file_hash_cache.h>

#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/platform.h>
#include <base/string_utils.h>

#include <sys/stat.h>

namespace dist_clang {
namespace cache {

i64 FileHashCache::Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool FileHashCache::Hash(const String& path, String* hash, String* error,
                         bool* time_macros, i64* modified, i64 fresh_since) {
  if (fresh_since) {
    std::lock_guard<Mutex> lock(mutex_);
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.checked >= fresh_since) {
      *hash = it->second.hash;
      if (time_macros) {
        *time_macros = it->second.time_macros;
      }
      if (modified) {
        *modified = std::max(it->second.mtime_ns, it->second.ctime_ns);
      }
      return true;
    }
  }

  // Any change before this time is seen by the check.
  const i64 checked = Now();
  struct stat st;
  if (stat(path.c_str(), &st) == -1) {
    base::GetLastError(error);
    std::lock_guard<Mutex> lock(mutex_);
    entries_.erase(path);
    return false;
  }

  if (modified) {
    *modified = std::max(base::ModificationTime(st), base::StatusChangeTime(st));
  }

  const String& key = path;
  bool hashing = false;
  {
    std::unique_lock<Mutex> lock(mutex_);
    auto TakeMemoized = [&] {
      auto it = entries_.find(key);
      if (it != entries_.end() && it->second.inode == st.st_ino &&
          it->second.size == static_cast<ui64>(st.st_size) &&
          it->second.mtime_ns == base::ModificationTime(st) &&
          it->second.ctime_ns == base::StatusChangeTime(st)) {
        it->second.checked = std::max(it->second.checked, checked);
        *hash = it->second.hash;
        if (time_macros) {
          *time_macros = it->second.time_macros;
        }
        return true;
      }
      return false;
    };
    if (TakeMemoized()) {
      return true;
    }
    // The same file is hashed by another lookup - e.g. a common header after
    // the start of the daemon: its hash is taken, instead of reading the file
    // again. A file, that isn't memoized after all, is read by each.
    if (hashing_.contains(key)) {
      hashed_.wait(lock, [&] { return !hashing_.contains(key); });
      if (TakeMemoized()) {
        return true;
      }
    }
    hashing = hashing_.insert(key).second;
  }
  // Lets the waiting lookups go on any return.
  auto done = [this, &key](bool* hashing) {
    if (*hashing) {
      {
        std::lock_guard<Mutex> lock(mutex_);
        hashing_.erase(key);
      }
      hashed_.notify_all();
    }
  };
  std::unique_ptr<bool, decltype(done)> done_guard(&hashing, done);

  Immutable content;
  if (!ReadFile(path, &content, error)) {
    return false;
  }
  // The contents must match the time of the modification.
  struct stat new_st;
  if (stat(path.c_str(), &new_st) == -1 || new_st.st_ino != st.st_ino ||
      new_st.st_size != st.st_size ||
      base::ModificationTime(new_st) != base::ModificationTime(st) ||
      base::StatusChangeTime(new_st) != base::StatusChangeTime(st)) {
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

  const i64 now = base::FileSystemTime();
  if (now - base::ModificationTime(st) > racy_interval_ns_ &&
      now - base::StatusChangeTime(st) > racy_interval_ns_) {
    std::lock_guard<Mutex> lock(mutex_);
    // Simpler than LRU, and the overflow is rare: only the hashes of the
    // current builds are computed again.
    if (entries_.size() >= max_entries_ && !entries_.contains(key)) {
      entries_.clear();
    }
    entries_[key] = {static_cast<ui64>(st.st_ino),
                     static_cast<ui64>(st.st_size), base::ModificationTime(st),
                     base::StatusChangeTime(st), *hash, has_time_macros,
                     checked};
  }

  return true;
}

bool FileHashCache::ReadFile(const String& path, Immutable* content,
                             String* error) const {
  return base::File::Read(path, content, error);
}

size_t FileHashCache::size() const {
  std::lock_guard<Mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace cache
}  // namespace dist_clang
