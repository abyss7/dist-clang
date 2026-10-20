#include <cache/tag_index.h>

#include <base/assert.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/sqlite3.h>
#include <cache/sqlite_utils.h>

#include <base/using_log.h>

namespace dist_clang {
namespace cache {

namespace {

// The tags are joined by the new lines - they can't contain them.
String JoinTags(const Vector<String>& tags) {
  String result;
  for (const auto& tag : tags) {
    result += (result.empty() ? "" : "\n") + tag;
  }
  return result;
}

bool Exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  auto result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    LOG(DB_ERROR) << sqlite3_errstr(result) << ": " << error;
    sqlite3_free(error);
    return false;
  }
  return true;
}

}  // namespace

TagIndex::TagIndex(const String& path, const String& name)
    : path_(path + "/" + name + ".sqlite") {
  db_ = OpenDatabase(path_);
  CHECK(Exec(db_,
             "PRAGMA journal_mode=WAL;"
             "PRAGMA synchronous=NORMAL;"
             "CREATE TABLE IF NOT EXISTS tags("
             "    entry TEXT PRIMARY KEY NOT NULL,"
             "    tags TEXT NOT NULL"
             ");"
             "CREATE TABLE IF NOT EXISTS times("
             "    entry TEXT NOT NULL,"
             "    tag TEXT NOT NULL,"
             "    time INTEGER NOT NULL,"
             "    PRIMARY KEY (entry, tag)"
             ") WITHOUT ROWID;"))
      << "Failed to create the table of tags";
}

TagIndex::~TagIndex() {
  sqlite3_close(db_);
}

bool TagIndex::Set(const String& entry, const Vector<String>& tags) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT OR REPLACE INTO tags (entry, tags) "
                         "VALUES (?, ?)",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  const String joined = JoinTags(tags);
  sqlite3_bind_text(stmt, 1, entry.data(), entry.size(), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, joined.data(), joined.size(), SQLITE_TRANSIENT);
  const bool result = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return result;
}

bool TagIndex::Delete(const String& entry) {
  bool result = true;
  for (const char* sql : {"DELETE FROM tags WHERE entry = ?",
                          "DELETE FROM times WHERE entry = ?"}) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text(stmt, 1, entry.data(), entry.size(), SQLITE_TRANSIENT);
    result &= sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
  }
  return result;
}

HashMap<String, Vector<String>> TagIndex::GetAll() const {
  HashMap<String, Vector<String>> result;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT entry, tags FROM tags", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return result;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const String entry(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
        sqlite3_column_bytes(stmt, 0));
    const String tags(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
        sqlite3_column_bytes(stmt, 1));
    List<String> tag_list;
    base::SplitString<'\n'>(tags, tag_list);
    result[entry].assign(tag_list.begin(), tag_list.end());
  }
  sqlite3_finalize(stmt);
  return result;
}

bool TagIndex::SetTime(const String& entry, const String& tag, ui64 time) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT OR REPLACE INTO times (entry, tag, time) "
                         "VALUES (?, ?, ?)",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, entry.data(), entry.size(), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, tag.data(), tag.size(), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, sqlite3_int64(time));
  const bool result = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return result;
}

bool TagIndex::DeleteTime(const String& entry, const String& tag) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM times WHERE entry = ? AND tag = ?",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, entry.data(), entry.size(), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, tag.data(), tag.size(), SQLITE_TRANSIENT);
  const bool result = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return result;
}

HashMap<String, HashMap<String, ui64>> TagIndex::GetAllTimes() const {
  HashMap<String, HashMap<String, ui64>> result;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT entry, tag, time FROM times", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return result;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const String entry(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
        sqlite3_column_bytes(stmt, 0));
    const String tag(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
        sqlite3_column_bytes(stmt, 1));
    result[tag][entry] = ui64(sqlite3_column_int64(stmt, 2));
  }
  sqlite3_finalize(stmt);
  return result;
}

ui64 TagIndex::SizeOnDisk() const {
  ui64 size = 0;
  for (const auto& suffix : {"", "-wal", "-shm"}) {
    if (base::File::Exists(path_ + suffix)) {
      size += base::File::Size(path_ + suffix);
    }
  }
  return size;
}

bool TagIndex::BeginTransaction() {
  return Exec(db_, "BEGIN TRANSACTION");
}

bool TagIndex::EndTransaction() {
  return Exec(db_, "END TRANSACTION");
}

}  // namespace cache
}  // namespace dist_clang
