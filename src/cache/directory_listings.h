#pragma once

#include <base/attributes.h>
#include <base/types.h>

namespace dist_clang {
namespace cache {

// The names in the directories - for the checks of the shadowing of the
// headers: most of the paths, that could shadow them, don't exist, and the
// directories are listed instead of the checks of every path. A listing is
// remembered, while the directory isn't modified, unless it's modified just
// now: the names may change within the same tick of the clock of the file
// system.
class DirectoryListings {
 public:
  using Listing = HashSet<String>;

  // Returns the names in the directory |path|, or null, if it isn't a
  // directory.
  SharedPtr<const Listing> Get(const String& path) THREAD_SAFE;

 private:
  struct Entry {
    ui64 inode;
    i64 mtime_ns, ctime_ns;
    SharedPtr<const Listing> listing;
  };

  Mutex mutex_;
  HashMap<String, Entry> entries_;
};

}  // namespace cache
}  // namespace dist_clang
