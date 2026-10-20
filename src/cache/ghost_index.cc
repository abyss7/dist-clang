#include <cache/ghost_index.h>

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

Vector<String> SplitTags(const String& joined) {
  List<String> tags;
  if (!joined.empty()) {
    base::SplitString<'\n'>(joined, tags);
  }
  return Vector<String>(tags.begin(), tags.end());
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

// Finalizes the statement at the end of the scope.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      LOG(DB_ERROR) << "Failed to prepare: " << sql;
      stmt_ = nullptr;
    }
  }
  ~Statement() { sqlite3_finalize(stmt_); }

  explicit operator bool() const { return stmt_; }

  void Bind(int index, const String& value) {
    sqlite3_bind_text(stmt_, index, value.data(), value.size(),
                      SQLITE_TRANSIENT);
  }
  void Bind(int index, ui64 value) {
    sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
  }

  // Returns |true| on a row.
  bool Step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
  bool Done() { return sqlite3_step(stmt_) == SQLITE_DONE; }

  ui64 Integer(int column) const {
    return static_cast<ui64>(sqlite3_column_int64(stmt_, column));
  }
  String Text(int column) const {
    return String(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column)),
        sqlite3_column_bytes(stmt_, column));
  }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

GhostIndex::GhostIndex(const String& path, const String& name, bool full_check)
    : path_(path + "/" + name + ".sqlite") {
  db_ = OpenDatabase(path_, nullptr, full_check);
  CHECK(Exec(db_,
             "PRAGMA journal_mode=WAL;"
             "PRAGMA synchronous=NORMAL;"
             "CREATE TABLE IF NOT EXISTS ghosts("
             "    object TEXT PRIMARY KEY NOT NULL,"
             "    size INTEGER NOT NULL,"
             "    compilation_time INTEGER NOT NULL,"
             "    removed INTEGER NOT NULL,"
             "    reason INTEGER NOT NULL,"
             "    tags TEXT NOT NULL,"
             "    evicted_before INTEGER NOT NULL"
             ");"
             "CREATE INDEX IF NOT EXISTS ghosts_removed ON ghosts(removed);"
             "CREATE TABLE IF NOT EXISTS losses("
             "    id INTEGER PRIMARY KEY,"
             "    time INTEGER NOT NULL,"
             "    reason INTEGER NOT NULL,"
             "    compilation_time INTEGER NOT NULL,"
             "    extra_size INTEGER NOT NULL,"
             "    tags TEXT NOT NULL"
             ");"
             // The size of all evicted entries - only grows.
             "CREATE TABLE IF NOT EXISTS counters("
             "    name TEXT PRIMARY KEY NOT NULL,"
             "    value INTEGER NOT NULL"
             ");"))
      << "Failed to create the tables of the removed entries";
}

GhostIndex::~GhostIndex() {
  sqlite3_close(db_);
}

ui64 GhostIndex::EvictedSize() const {
  Statement stmt(db_, "SELECT value FROM counters WHERE name = 'evicted'");
  return stmt && stmt.Step() ? stmt.Integer(0) : 0;
}

bool GhostIndex::Add(const Ghost& ghost, ui64 time) {
  const ui64 evicted_before = EvictedSize();
  if (ghost.reason == EVICTED) {
    Statement stmt(db_,
                   "INSERT OR REPLACE INTO counters (name, value) "
                   "VALUES ('evicted', ?)");
    if (!stmt) {
      return false;
    }
    stmt.Bind(1, evicted_before + ghost.size);
    if (!stmt.Done()) {
      return false;
    }
  }

  Statement stmt(db_,
                 "INSERT OR REPLACE INTO ghosts (object, size, "
                 "compilation_time, removed, reason, tags, evicted_before) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
  if (!stmt) {
    return false;
  }
  stmt.Bind(1, ghost.object);
  stmt.Bind(2, ghost.size);
  stmt.Bind(3, ghost.compilation_time);
  stmt.Bind(4, time);
  stmt.Bind(5, ui64(ghost.reason));
  stmt.Bind(6, JoinTags(ghost.tags));
  stmt.Bind(7, evicted_before);
  return stmt.Done();
}

bool GhostIndex::Resurrect(const String& object, ui64 compilation_time,
                           ui64 time) {
  Loss loss;
  String tags;
  {
    Statement stmt(db_,
                   "SELECT compilation_time, reason, tags, evicted_before "
                   "FROM ghosts WHERE object = ?");
    if (!stmt) {
      return false;
    }
    stmt.Bind(1, object);
    if (!stmt.Step()) {
      return false;
    }
    loss.compilation_time = compilation_time ? compilation_time
                                             : stmt.Integer(0);
    loss.reason = static_cast<Reason>(stmt.Integer(1));
    tags = stmt.Text(2);
    if (loss.reason == EVICTED) {
      loss.extra_size = EvictedSize() - stmt.Integer(3);
    }
  }

  {
    Statement stmt(db_,
                   "INSERT INTO losses (time, reason, compilation_time, "
                   "extra_size, tags) VALUES (?, ?, ?, ?, ?)");
    if (!stmt) {
      return false;
    }
    stmt.Bind(1, time);
    stmt.Bind(2, ui64(loss.reason));
    stmt.Bind(3, loss.compilation_time);
    stmt.Bind(4, loss.extra_size);
    stmt.Bind(5, tags);
    if (!stmt.Done()) {
      return false;
    }
  }

  Statement stmt(db_, "DELETE FROM ghosts WHERE object = ?");
  if (stmt) {
    stmt.Bind(1, object);
    stmt.Done();
  }
  return true;
}

ui64 GhostIndex::Count() const {
  Statement stmt(db_, "SELECT COUNT(*) FROM ghosts");
  return stmt && stmt.Step() ? stmt.Integer(0) : 0;
}

List<GhostIndex::Loss> GhostIndex::GetLosses() const {
  List<Loss> result;
  Statement stmt(db_,
                 "SELECT reason, compilation_time, extra_size, tags "
                 "FROM losses ORDER BY time");
  if (!stmt) {
    return result;
  }
  while (stmt.Step()) {
    Loss loss;
    loss.reason = static_cast<Reason>(stmt.Integer(0));
    loss.compilation_time = stmt.Integer(1);
    loss.extra_size = stmt.Integer(2);
    loss.tags = SplitTags(stmt.Text(3));
    result.push_back(std::move(loss));
  }
  return result;
}

bool GhostIndex::RemoveOlderThan(ui64 time) {
  for (const char* sql : {"DELETE FROM ghosts WHERE removed < ?",
                          "DELETE FROM losses WHERE time < ?"}) {
    Statement stmt(db_, sql);
    if (!stmt) {
      return false;
    }
    stmt.Bind(1, time);
    if (!stmt.Done()) {
      return false;
    }
  }
  return true;
}

ui64 GhostIndex::Clear() {
  const ui64 count = Count();
  if (!Exec(db_, "DELETE FROM ghosts; DELETE FROM losses;")) {
    return 0;
  }
  return count;
}

ui64 GhostIndex::SizeOnDisk() const {
  ui64 size = 0;
  for (const auto& suffix : {"", "-wal", "-shm"}) {
    if (base::File::Exists(path_ + suffix)) {
      size += base::File::Size(path_ + suffix);
    }
  }
  return size;
}

bool GhostIndex::BeginTransaction() {
  return Exec(db_, "BEGIN TRANSACTION");
}

bool GhostIndex::EndTransaction() {
  return Exec(db_, "END TRANSACTION");
}

}  // namespace cache
}  // namespace dist_clang
