#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <cache/database.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Simple persistent key-value storage on top of SQLite.
class SQLiteKV : public Database<Immutable> {
 public:
  // The |full_check| of the database - see |OpenDatabase()|.
  SQLiteKV(const String& path, const String& name, bool full_check = true);
  ~SQLiteKV() override;

  bool Set(const String& key, const Immutable& value) override THREAD_SAFE;
  bool Get(const String& key, Immutable* value) const override THREAD_SAFE;
  bool Delete(const String& key) override THREAD_SAFE;

  inline ui32 GetVersion() const override {
    // No versioning for now.
    return 0;
  }

  ui64 SizeOnDisk() const;
  ui64 Count() const THREAD_SAFE;

  // Removes the rows, that are |dead|. Returns their number.
  ui64 DeleteIf(const Fn<bool(const String& key, const String& value)>& dead)
      THREAD_SAFE;

 private:
  sqlite3* db_ = nullptr;
  const String path_;
};

}  // namespace cache
}  // namespace dist_clang
