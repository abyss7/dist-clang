#pragma once

#include <base/types.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Persistent tags of the cache entries - e.g. the branches, that used them.
class TagIndex {
 public:
  TagIndex(const String& path, const String& name);
  ~TagIndex();

  // Replaces the tags of the |entry|.
  bool Set(const String& entry, const Vector<String>& tags);
  bool Delete(const String& entry);

  HashMap<String, Vector<String>> GetAll() const;

  bool BeginTransaction();
  bool EndTransaction();

  ui64 SizeOnDisk() const;

 private:
  const String path_;
  sqlite3* db_ = nullptr;
};

}  // namespace cache
}  // namespace dist_clang
