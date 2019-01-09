#pragma once

#include <base/types.h>
#include <cache/database.h>

#undef VERSION  // FIXME: is it necessery?

struct sqlite3;
struct sqlite3_stmt;

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
  SQLite(const String& path, const String& name);
  ~SQLite() override;

  bool Get(const String& key, Value* value) const override;
  bool Set(const String& key, const Value& value) override;
  bool Delete(const String& key) override;

  ui32 GetVersion() const override;

  bool First(Immutable* hash, Value* value) const;

  bool BeginTransaction();
  bool EndTransaction();

 private:
  bool Migrate() const;

  sqlite3* db_ = nullptr;
  const String path_;

  const ui32 kSQLiteVersion = 0;
};

}  // namespace cache
}  // namespace dist_clang
