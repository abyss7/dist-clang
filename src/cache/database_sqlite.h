#pragma once

#include <base/types.h>
#include <cache/database.h>

#undef VERSION  // FIXME: is it necessery?

struct sqlite3;
struct sqlite3_stmt;

namespace dist_clang {
namespace cache {

// The index of the cache entries.
class SQLite
    : public Database<size_t /* mtime */, size_t /* size */,
                      ui32 /* version */, ui32 /* hits */, ui32 /* direct */,
                      String /* blobs */> {
 public:
  enum Field {
    MTIME = 0,
    SIZE,
    VERSION,
    HITS,
    DIRECT,
    // If the entry is a manifest of the direct cache.
    BLOBS,
    // The hashes of the blobs of the entry, separated by the new lines.

    MAX_FIELD_VALUE,
    // special value - don't place anything beyond it.
  };

  SQLite();
  SQLite(const String& path, const String& name);
  ~SQLite() override;

  bool Get(const String& key, Value* value) const override;
  bool Set(const String& key, const Value& value) override;
  bool Delete(const String& key) override;

  inline ui32 GetVersion() const override { return 0; }

  // If the index is new: it didn't exist, or was broken.
  inline bool created() const { return created_; }

  // The hits keep an entry only for this time since its last use - otherwise
  // the entries of the abandoned branches, hit once, would outlive the fresh
  // ones.
  enum : size_t { kHitsTtl = 7 * 24 * 60 * 60 };

  // All entries in the order of eviction: never hit entries, and the ones not
  // used for |kHitsTtl|, go first, then the least recently used ones.
  List<Pair<String, Value>> GetInEvictionOrder() const;

  bool BeginTransaction();
  bool EndTransaction();

 private:
  void CreateTable();

  sqlite3* db_ = nullptr;
  const String path_;
  bool created_ = true;
};

}  // namespace cache
}  // namespace dist_clang
