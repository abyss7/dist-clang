#pragma once

#include <base/aliases.h>
#include <cache/database.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

class SQLite : public Database<size_t /* mtime */, size_t /* size */,
                               ui32 /* version */> {
 public:
  enum Field {
    MTIME = 0,
    SIZE,
    VERSION,

    MAX_FIELD_VALUE,
    // special value - don't place anything beyond it.
  };

  SQLite();
  ~SQLite() override;

  bool Get(const String& key, Value* value) const override;
  bool Set(const String& key, const Value& value) override;
  bool Delete(const String& key) override;

  ui32 GetVersion() const override;

  bool First(Immutable* hash, Value* value) const;

 private:
  sqlite3* db_ = nullptr;

  const ui32 kSQLiteVersion = 0;
};

}  // namespace cache
}  // namespace dist_clang
