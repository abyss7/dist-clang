#include <cache/database_kv.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <cache/sqlite3.h>
#include <cache/sqlite_utils.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

// Finalizes the statement on scope exit.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    auto result = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    if (result != SQLITE_OK) {
      LOG(DB_ERROR) << "Failed to prepare SQL statement with error: "
                    << sqlite3_errmsg(db);
      stmt_ = nullptr;
    }
  }

  ~Statement() {
    if (stmt_) {
      sqlite3_finalize(stmt_);
    }
  }

  inline operator sqlite3_stmt*() const { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

SQLiteKV::SQLiteKV(const String& path, const String& name, bool full_check)
    : path_(path + "/" + name + ".sqlite") {
  db_ = OpenDatabase(path_, nullptr, full_check);

  char* error = nullptr;
  auto result = sqlite3_exec(db_,
                        "PRAGMA journal_mode=WAL;"
                        "PRAGMA synchronous=NORMAL;"
                        "CREATE TABLE IF NOT EXISTS kv("
                        "    key TEXT PRIMARY KEY NOT NULL,"
                        "    value BLOB NOT NULL"
                        ");",
                        nullptr, nullptr, &error);
  CHECK(result == SQLITE_OK) << "Failed to create table: "
                             << sqlite3_errstr(result) << ": " << error;

  LOG(DB_INFO) << "Database is opened on path " << path_;
}

SQLiteKV::~SQLiteKV() {
  auto result = sqlite3_close(db_);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to close database: " << sqlite3_errstr(result);
  }
}

bool SQLiteKV::Set(const String& key, const Immutable& value) {
  Statement stmt(db_, "INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?)");
  if (!stmt) {
    return false;
  }

  Immutable non_const_value = value;
  sqlite3_bind_text(stmt, 1, key.data(), key.size(), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, non_const_value.data(), non_const_value.size(),
                    SQLITE_TRANSIENT);

  auto result = sqlite3_step(stmt);
  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to set " << key << " => " << value
                  << " with error: " << sqlite3_errstr(result);
    return false;
  }

  LOG(DB_VERBOSE) << "Database set " << key << " => " << value;
  return true;
}

bool SQLiteKV::Get(const String& key, Immutable* value) const {
  DCHECK(value);

  Statement stmt(db_, "SELECT value FROM kv WHERE key = ?");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, key.data(), key.size(), SQLITE_TRANSIENT);

  auto result = sqlite3_step(stmt);
  if (result == SQLITE_ROW) {
    const auto* data =
        reinterpret_cast<const char*>(sqlite3_column_blob(stmt, 0));
    const auto size = sqlite3_column_bytes(stmt, 0);
    value->assign(Immutable(String(data, size)));
    return true;
  }

  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to get " << key
                  << " with error: " << sqlite3_errstr(result);
  }

  return false;
}

bool SQLiteKV::Delete(const String& key) {
  Statement stmt(db_, "DELETE FROM kv WHERE key = ?");
  if (!stmt) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, key.data(), key.size(), SQLITE_TRANSIENT);

  auto result = sqlite3_step(stmt);
  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to delete " << key
                  << " with error: " << sqlite3_errstr(result);
    return false;
  }

  return true;
}

ui64 SQLiteKV::Count() const {
  Statement stmt(db_, "SELECT COUNT(*) FROM kv");
  return stmt && sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0)
                                                  : 0;
}

ui64 SQLiteKV::DeleteIf(
    const Fn<bool(const String& key, const String& value)>& dead) {
  List<String> keys;
  {
    Statement stmt(db_, "SELECT key, value FROM kv");
    if (!stmt) {
      return 0;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const String key(
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
          sqlite3_column_bytes(stmt, 0));
      const String value(
          reinterpret_cast<const char*>(sqlite3_column_blob(stmt, 1)),
          sqlite3_column_bytes(stmt, 1));
      if (dead(key, value)) {
        keys.push_back(key);
      }
    }
  }
  sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
  for (const auto& key : keys) {
    Delete(key);
  }
  sqlite3_exec(db_, "END TRANSACTION", nullptr, nullptr, nullptr);
  return keys.size();
}

ui64 SQLiteKV::SizeOnDisk() const {
  ui64 size = 0;
  for (const auto& suffix : {"", "-wal", "-shm"}) {
    const auto file_path = path_ + suffix;
    if (base::File::Exists(file_path)) {
      size += base::File::Size(file_path);
    }
  }
  return size;
}

}  // namespace cache
}  // namespace dist_clang
