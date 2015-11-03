#include <cache/database_sqlite.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/sqlite3.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

bool TableExists(sqlite3* db, const String& name) {
  sqlite3_stmt* stmt;
  const String sql =
      "SELECT name FROM sqlite_master WHERE type='table' AND name = '" + name +
      "'";
  auto result = sqlite3_prepare_v2(db, sql.c_str(), sql.size(), &stmt, nullptr);
  DCHECK(result == SQLITE_OK);

  result = sqlite3_step(stmt);
  bool res = result == SQLITE_ROW;

  result = sqlite3_finalize(stmt);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to finalize SQL statement with error: "
                  << sqlite3_errstr(result);
  }

  return res;
}

}  // namespace

SQLite::SQLite() : path_(":memory:") {
  auto result = sqlite3_open(path_.c_str(), &db_);
  CHECK(result == SQLITE_OK) << "Failed to open in-memory database: "
                             << sqlite3_errstr(result);

  char* error;
  // FIXME: 50 is a magical constant - it's the length of the hash string.
  result = sqlite3_exec(db_,
                        "CREATE TABLE entries("
                        "    hash CHAR(50) PRIMARY KEY NOT NULL,"
                        "    mtime INT NOT NULL,"
                        "    size INT NOT NULL,"
                        "    version INT NOT NULL"
                        ");",
                        nullptr, nullptr, &error);
  CHECK(result == SQLITE_OK)
      << "Failed to create table: " << sqlite3_errstr(result) << ": " << error;

  result = sqlite3_exec(db_, "CREATE INDEX mtime_idx ON entries (mtime);",
                        nullptr, nullptr, &error);
  CHECK(result == SQLITE_OK)
      << "Failed to create index: " << sqlite3_errstr(result) << ": " << error;

  LOG(DB_INFO) << "SQLite database is created in-memory";
}

SQLite::SQLite(const String& path, const String& name)
    : path_(path + "/" + name + ".sqlite") {
  auto result = sqlite3_open(path_.c_str(), &db_);
  CHECK(result == SQLITE_OK) << "Failed to open " << path_ << ": "
                             << sqlite3_errstr(result);

  if (TableExists(db_, "entries")) {
    // TODO: do migration.
    LOG(DB_INFO) << "SQLite database is opened on path " << path_;
  } else {
    char* error;
    // FIXME: 50 is a magical constant - it's the length of the hash string.
    result = sqlite3_exec(db_,
                          "CREATE TABLE entries("
                          "    hash CHAR(50) PRIMARY KEY NOT NULL,"
                          "    mtime INT NOT NULL,"
                          "    size INT NOT NULL,"
                          "    version INT NOT NULL"
                          ");",
                          nullptr, nullptr, &error);
    CHECK(result == SQLITE_OK)
        << "Failed to create table: " << sqlite3_errstr(result) << ": "
        << error;

    result = sqlite3_exec(db_, "CREATE INDEX mtime_idx ON entries (mtime);",
                          nullptr, nullptr, &error);
    CHECK(result == SQLITE_OK)
        << "Failed to create index: " << sqlite3_errstr(result) << ": "
        << error;

    LOG(DB_INFO) << "SQLite database is created on path " << path_;
  }
}

SQLite::~SQLite() {
  auto result = sqlite3_close(db_);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to close database: " << sqlite3_errstr(result);
  }
}

bool SQLite::Get(const String& key, Value* value) const {
  DCHECK(value);

  sqlite3_stmt* stmt;
  const String sql =
      "SELECT mtime, size, version FROM entries WHERE hash = '" + key + "'";
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
    std::get<VERSION>(*value) = sqlite3_column_int64(stmt, VERSION);
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
      "INSERT OR REPLACE INTO entries (hash, mtime, size, version) VALUES ('" +
      key + "', " + std::to_string(std::get<MTIME>(value)) + ", " +
      std::to_string(std::get<SIZE>(value)) + ", " +
      std::to_string(std::get<VERSION>(value)) + ");";
  auto result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    return false;
  }

  return true;
}

bool SQLite::Delete(const String& key) {
  char* error;
  const String sql = "DELETE FROM entries WHERE hash = '" + key + "'";
  auto result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    return false;
  }

  return true;
}

ui32 SQLite::GetVersion() const {
  sqlite3_stmt* stmt;
  const String sql = "PRAGMA user_version";
  auto result =
      sqlite3_prepare_v2(db_, sql.c_str(), sql.size(), &stmt, nullptr);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to prepare SQL statement with error: "
                  << sqlite3_errmsg(db_);
    return 0;
  }

  result = sqlite3_step(stmt);
  CHECK(result == SQLITE_ROW);

  return sqlite3_column_int64(stmt, 0);
}

bool SQLite::First(Immutable* hash, Value* value) const {
  DCHECK(hash);
  DCHECK(value);

  sqlite3_stmt* stmt;
  const String sql =
      "SELECT mtime, size, version, hash FROM entries ORDER BY mtime ASC";
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
    std::get<VERSION>(*value) = sqlite3_column_int64(stmt, VERSION);

    auto column_count = sqlite3_column_count(stmt);
    DCHECK(column_count > MAX_FIELD_VALUE);

    hash->assign(String(reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, column_count - 1))));
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
