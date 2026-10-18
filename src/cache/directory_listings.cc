#include <cache/directory_listings.h>

#include <base/platform.h>

#include <dirent.h>
#include <sys/stat.h>

namespace dist_clang {
namespace cache {

namespace {

// The directories modified so recently may still change within the same
// timestamp - see |FileHashCache|.
const i64 kRacyIntervalNs = 2'000'000'000;

}  // namespace

SharedPtr<const DirectoryListings::Listing> DirectoryListings::Get(
    const String& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return nullptr;
  }
  const i64 mtime = base::ModificationTime(st);
  const i64 ctime = base::StatusChangeTime(st);
  {
    std::lock_guard<Mutex> lock(mutex_);
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.inode == ui64(st.st_ino) &&
        it->second.mtime_ns == mtime && it->second.ctime_ns == ctime) {
      return it->second.listing;
    }
  }

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return nullptr;
  }
  auto listing = std::make_shared<Listing>();
  while (auto* entry = readdir(dir)) {
    // The names are compared like the file system does - e.g. ignoring the
    // case on Mac.
    listing->emplace(base::PathKey(entry->d_name));
  }
  closedir(dir);

  const i64 now = base::FileSystemTime();
  if (now - mtime > kRacyIntervalNs && now - ctime > kRacyIntervalNs) {
    std::lock_guard<Mutex> lock(mutex_);
    entries_[path] = {ui64(st.st_ino), mtime, ctime, listing};
  }
  return listing;
}

}  // namespace cache
}  // namespace dist_clang
