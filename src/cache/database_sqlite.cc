#include <cache/database_sqlite.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/sqlite3.h>
#include <cache/sqlite_utils.h>

#include <time.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

// Finalizes the statement on scope exit.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
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

SQLite::Value ReadValue(sqlite3_stmt* stmt, int first_column) {
  SQLite::Value value;
  std::get<SQLite::MTIME>(value) = sqlite3_column_int64(stmt, first_column);
  std::get<SQLite::SIZE>(value) = sqlite3_column_int64(stmt, first_column + 1);
  std::get<SQLite::VERSION>(value) =
      sqlite3_column_int64(stmt, first_column + 2);
  std::get<SQLite::HITS>(value) = sqlite3_column_int64(stmt, first_column + 3);
  std::get<SQLite::DIRECT>(value) =
      sqlite3_column_int64(stmt, first_column + 4);
  const auto* blobs =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, first_column + 5));
  std::get<SQLite::BLOBS>(value) = blobs ? blobs : "";
  return value;
}

}  // namespace

SQLite::SQLite() : path_(":memory:") {
  auto result = sqlite3_open(path_.c_str(), &db_);
  CHECK(result == SQLITE_OK) << "Failed to open in-memory database: "
                             << sqlite3_errstr(result);
  CreateTable();
  LOG(DB_INFO) << "SQLite database is created in-memory";
}

SQLite::SQLite(const String& path, const String& name, bool full_check)
    : path_(path + "/" + name + ".sqlite") {
  db_ = OpenDatabase(path_, &created_, full_check);
  // A crash of the daemon loses nothing; a crash of the system may lose the
  // last transactions, but not the integrity - and the files are scanned
  // after any crash anyway.
  char* error = nullptr;
  auto result = sqlite3_exec(db_,
                             "PRAGMA journal_mode=WAL;"
                             "PRAGMA synchronous=NORMAL;",
                             nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to set up the journal of " << path_ << ": "
                  << sqlite3_errstr(result) << ": " << error;
    sqlite3_free(error);
  }
  CreateTable();
  LOG(DB_INFO) << "SQLite database is opened on path " << path_;
}

SQLite::~SQLite() {
  auto result = sqlite3_close(db_);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << "Failed to close database: " << sqlite3_errstr(result);
  }
}

void SQLite::CreateTable() {
  char* error = nullptr;
  // SQLite doesn't limit the length of the text: "CHAR(50)" is only its type.
  auto result = sqlite3_exec(db_,
                             "CREATE TABLE IF NOT EXISTS entries("
                             "    hash CHAR(50) PRIMARY KEY NOT NULL,"
                             "    mtime INT NOT NULL,"
                             "    size INT NOT NULL,"
                             "    version INT NOT NULL,"
                             "    hits INT NOT NULL DEFAULT 0,"
                             "    direct INT NOT NULL DEFAULT 0,"
                             "    blobs TEXT NOT NULL DEFAULT ''"
                             ");"
                             "CREATE INDEX IF NOT EXISTS mtime_idx "
                             "    ON entries (mtime);",
                             nullptr, nullptr, &error);
  CHECK(result == SQLITE_OK)
      << "Failed to create table: " << sqlite3_errstr(result) << ": " << error;
}

bool SQLite::Get(const String& key, Value* value) const {
  DCHECK(value);

  Statement stmt(db_,
                 "SELECT mtime, size, version, hits, direct, blobs "
                 "FROM entries WHERE hash = ?");
  if (!stmt) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, key.data(), key.size(), SQLITE_TRANSIENT);

  auto result = sqlite3_step(stmt);
  if (result == SQLITE_ROW) {
    *value = ReadValue(stmt, 0);
    return true;
  }
  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to get " << key
                  << " with error: " << sqlite3_errstr(result);
  }
  return false;
}

bool SQLite::Set(const String& key, const Value& value) {
  Statement stmt(db_,
                 "INSERT OR REPLACE INTO entries (hash, mtime, size, version, "
                 "hits, direct, blobs) VALUES (?, ?, ?, ?, ?, ?, ?)");
  if (!stmt) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, key.data(), key.size(), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, std::get<MTIME>(value));
  sqlite3_bind_int64(stmt, 3, std::get<SIZE>(value));
  sqlite3_bind_int64(stmt, 4, std::get<VERSION>(value));
  sqlite3_bind_int64(stmt, 5, std::get<HITS>(value));
  sqlite3_bind_int64(stmt, 6, std::get<DIRECT>(value));
  const auto& blobs = std::get<BLOBS>(value);
  sqlite3_bind_text(stmt, 7, blobs.data(), blobs.size(), SQLITE_TRANSIENT);

  auto result = sqlite3_step(stmt);
  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to set " << key
                  << " with error: " << sqlite3_errstr(result);
    return false;
  }
  return true;
}

bool SQLite::Delete(const String& key) {
  Statement stmt(db_, "DELETE FROM entries WHERE hash = ?");
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

List<Pair<String, SQLite::Value>> SQLite::GetInEvictionOrder() const {
  List<Pair<String, Value>> entries;

  Statement stmt(db_,
                 "SELECT hash, mtime, size, version, hits, direct, blobs "
                 "FROM entries ORDER BY hits > 0 AND mtime >= ?, mtime ASC");
  if (!stmt) {
    return entries;
  }
  const i64 now = time(nullptr);
  sqlite3_bind_int64(stmt, 1, now - kHitsTtl);

  int result;
  while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
    entries.emplace_back(
        String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))),
        ReadValue(stmt, 1));
  }
  if (result != SQLITE_DONE) {
    LOG(DB_ERROR) << "Failed to list entries with error: "
                  << sqlite3_errstr(result);
  }

  return entries;
}

bool SQLite::BeginTransaction() {
  char* error = nullptr;
  auto result =
      sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    sqlite3_free(error);
    return false;
  }

  return true;
}

bool SQLite::EndTransaction() {
  char* error = nullptr;
  auto result = sqlite3_exec(db_, "END TRANSACTION", nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    sqlite3_free(error);
    return false;
  }

  return true;
}

}  // namespace cache
}  // namespace dist_clang
