#pragma once

#include <base/thread_pool.h>

namespace dist_clang {
namespace daemon {

class FileCache {
 public:
  enum {
    UNLIMITED = 0
  };
  using Entry = Pair<String /* path, stderr */>;
  using Optional = base::ThreadPool::Optional;

  explicit FileCache(const String& path);
  FileCache(const String& path, ui64 size);

  bool Find(const String& code, const String& command_line,
            const String& version, Entry* entry) const;
  Optional Store(const String& code, const String& command_line,
                 const String& version, const Entry& entry);
  void SyncStore(const String& code, const String& command_line,
                 const String& version, const Entry& entry);

 private:
  const String path_;
  base::ThreadPool pool_;
  ui64 max_size_, cached_size_;

  void DoStore(const String& path, const String& code_hash, const Entry& entry);
};

}  // namespace daemon
}  // namespace dist_clang
