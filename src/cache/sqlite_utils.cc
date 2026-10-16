#include <cache/sqlite_utils.h>

#include <base/assert.h>
#include <base/logging.h>
#include <cache/sqlite3.h>

#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

bool IsIntact(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return false;
  }

  bool intact = sqlite3_step(stmt) == SQLITE_ROW &&
                String(reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, 0))) == "ok";
  sqlite3_finalize(stmt);
  return intact;
}

}  // namespace

sqlite3* OpenDatabase(const String& path, bool* created) {
  if (created) {
    *created = access(path.c_str(), F_OK) != 0;
  }
  sqlite3* db = nullptr;
  auto result = sqlite3_open_v2(
      path.c_str(), &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (result == SQLITE_OK && IsIntact(db)) {
    return db;
  }

  LOG(DB_ERROR) << "Database " << path << " is broken - recreating it: "
                << (db ? sqlite3_errmsg(db) : sqlite3_errstr(result));
  sqlite3_close(db);
  if (created) {
    *created = true;
  }
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    unlink((path + suffix).c_str());
  }

  db = nullptr;
  result = sqlite3_open_v2(
      path.c_str(), &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  CHECK(result == SQLITE_OK) << "Failed to open " << path << ": "
                             << sqlite3_errstr(result);
  return db;
}

}  // namespace cache
}  // namespace dist_clang
