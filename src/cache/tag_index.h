#pragma once

#include <base/types.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Persistent tags of the cache entries - e.g. the branches, that used them.
class TagIndex {
 public:
  // The |full_check| of the database - see |OpenDatabase()|.
  TagIndex(const String& path, const String& name, bool full_check = true);
  ~TagIndex();

  // Replaces the tags of the |entry|.
  bool Set(const String& entry, const Vector<String>& tags);
  bool Delete(const String& entry);

  HashMap<String, Vector<String>> GetAll() const;

  // The time of the last use of the entry by the tag - in seconds.
  bool SetTime(const String& entry, const String& tag, ui64 time);
  bool DeleteTime(const String& entry, const String& tag);
  // Tag -> entry -> time.
  HashMap<String, HashMap<String, ui64>> GetAllTimes() const;

  bool BeginTransaction();
  bool EndTransaction();

  ui64 SizeOnDisk() const;

 private:
  const String path_;
  sqlite3* db_ = nullptr;
};

}  // namespace cache
}  // namespace dist_clang
