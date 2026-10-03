#pragma once

#include <base/attributes.h>
#include <base/types.h>

namespace dist_clang {
namespace cache {

// Memoizes hashes of file contents. A hash is reused while the file's inode,
// size, mtime and ctime stay the same.
class FileHashCache {
 public:
  // Returns the hex-encoded hash of the contents of |path|.
  bool Hash(const Path& path, String* hash, String* error = nullptr) THREAD_SAFE;

 private:
  struct Entry {
    ui64 inode, size;
    i64 mtime_ns, ctime_ns;
    String hash;
  };

  Mutex mutex_;
  HashMap<String, Entry> entries_;
};

}  // namespace cache
}  // namespace dist_clang
