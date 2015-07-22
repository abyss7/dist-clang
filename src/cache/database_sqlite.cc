#include <cache/database_sqlite.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/sqlite3.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

SQLite::SQLite() {
  auto result = sqlite3_open(":memory:", &db_);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(FATAL) << sqlite3_errstr(result);
  }

  char* error;
  // FIXME: 50 is a magical constant - it's the length of the hash string.
  result = sqlite3_exec(db_,
                        "CREATE TABLE tmp("
                        "    hash CHAR(50) PRIMARY KEY NOT NULL,"
                        "    mtime INT NOT NULL,"
                        "    size INT NOT NULL"
                        ");",
                        nullptr, nullptr, &error);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(FATAL) << sqlite3_errstr(result) << ": " << error;
  }

  result = sqlite3_exec(db_, "CREATE INDEX mtime_idx ON tmp (mtime);", nullptr,
                        nullptr, &error);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(FATAL) << sqlite3_errstr(result) << ": " << error;
  }
}

SQLite::~SQLite() {
  auto result = sqlite3_close(db_);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(FATAL) << sqlite3_errstr(result);
  }
}

bool SQLite::Get(const String& key, Value* value) const {
  DCHECK(value);

  sqlite3_stmt* stmt;
  const String sql = "SELECT mtime, size FROM tmp WHERE hash = '" + key + "'";
  auto result =
      sqlite3_prepare_v2(db_, sql.c_str(), sql.size(), &stmt, nullptr);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to prepare SQL statement with error: "
                  << sqlite3_errmsg(db_);
    return false;
  }

  bool has_entry = false;
  result = sqlite3_step(stmt);
  if (result == SQLITE_ROW) {
    std::get<MTIME>(*value) = sqlite3_column_int64(stmt, MTIME);
    std::get<SIZE>(*value) = sqlite3_column_int64(stmt, SIZE);
    has_entry = true;
  } else if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to get " << key
                  << " with error: " << sqlite3_errstr(result);
  }

  result = sqlite3_finalize(stmt);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to finalize SQL statement with error: "
                  << sqlite3_errstr(result);
  }

  return has_entry;
}

bool SQLite::Set(const String& key, const Value& value) {
  char* error;
  const String sql =
      "INSERT OR REPLACE INTO tmp (hash, mtime, size) VALUES ('" + key + "', " +
      std::to_string(std::get<MTIME>(value)) + ", " +
      std::to_string(std::get<SIZE>(value)) + ");";
  auto result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    return false;
  }

  return true;
}

bool SQLite::Delete(const String& key) {
  char* error;
  const String sql = "DELETE FROM tmp WHERE hash = '" + key + "'";
  auto result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
  // FIXME: make this look like:
  //        CHECK(result == SQLITE_OK) << sqlite_errstr(result);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    return false;
  }

  return true;
}

bool SQLite::First(Immutable* hash, Value* value) const {
  DCHECK(hash);
  DCHECK(value);

  sqlite3_stmt* stmt;
  const String sql = "SELECT mtime, size, hash FROM tmp ORDER BY mtime ASC";
  auto result =
      sqlite3_prepare_v2(db_, sql.c_str(), sql.size(), &stmt, nullptr);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to prepare SQL statement with error: "
                  << sqlite3_errmsg(db_);
    return false;
  }

  bool has_entry = false;
  result = sqlite3_step(stmt);
  if (result == SQLITE_ROW) {
    std::get<MTIME>(*value) = sqlite3_column_int64(stmt, MTIME);
    std::get<SIZE>(*value) = sqlite3_column_int64(stmt, SIZE);
    hash->assign(
        String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))));
    has_entry = true;
  } else if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to get first entry with error: "
                  << sqlite3_errstr(result);
  }

  result = sqlite3_finalize(stmt);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to finalize SQL statement with error: "
                  << sqlite3_errstr(result);
  }

  return has_entry;
}

}  // namespace cache
}  // namespace dist_clang
