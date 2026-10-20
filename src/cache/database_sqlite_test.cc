#include <cache/database_sqlite.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>
#include <cache/sqlite3.h>
#include <cache/sqlite_utils.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace cache {

TEST(SQLiteTest, OldHitsDontKeepEntries) {
  const base::TemporaryDir temp_dir;
  SQLite database(temp_dir.path(), "index");
  const size_t now = time(nullptr);
  const size_t day = 24 * 60 * 60;

  // Hit, but long ago - like the entries of an abandoned branch.
  ASSERT_TRUE(database.Set("old_hit", {now - 90 * day, 1, 0, 1, 0, String()}));
  // Never hit, but fresh - e.g. replaced in the build directory.
  ASSERT_TRUE(database.Set("fresh", {now - 60 * 60, 1, 0, 0, 0, String()}));
  // Hit recently - stays the longest, though it's older than "fresh".
  ASSERT_TRUE(database.Set("recent_hit", {now - day, 1, 0, 1, 0, String()}));
  // Never hit and old - goes first.
  ASSERT_TRUE(database.Set("old", {now - 100 * day, 1, 0, 0, 0, String()}));

  List<String> order;
  for (const auto& [hash, value] : database.GetInEvictionOrder()) {
    order.push_back(hash);
  }
  EXPECT_EQ((List<String>{"old", "old_hit", "fresh", "recent_hit"}), order);
}

TEST(SQLiteTest, FailedTransactions) {
  const base::TemporaryDir temp_dir;
  SQLite database(temp_dir.path(), "index");

  // The messages of the errors are freed - the leak sanitizer checks.
  ASSERT_TRUE(database.BeginTransaction());
  EXPECT_FALSE(database.BeginTransaction());
  ASSERT_TRUE(database.EndTransaction());
  EXPECT_FALSE(database.EndTransaction());
}

TEST(SQLiteTest, WriteAheadLog) {
  const base::TemporaryDir temp_dir;
  SQLite database(temp_dir.path(), "index");

  // The index is written on every flush - without the rollback journal and
  // the syncs of each transaction, like the other databases.
  sqlite3* db = OpenDatabase(temp_dir.path() / "index.sqlite");
  ASSERT_TRUE(db);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, nullptr));
  ASSERT_EQ(SQLITE_ROW, sqlite3_step(stmt));
  EXPECT_EQ("wal", String(reinterpret_cast<const char*>(
                       sqlite3_column_text(stmt, 0))));
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

TEST(SQLiteTest, FullCheckOnlyOnDemand) {
  const base::TemporaryDir temp_dir;
  const String path = temp_dir.path() / "test.sqlite";

  // Many pages - the damage in the middle is found only by the full check.
  {
    sqlite3* db = OpenDatabase(path);
    ASSERT_EQ(SQLITE_OK,
              sqlite3_exec(db,
                           "CREATE TABLE t(v TEXT);"
                           "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL "
                           "SELECT i + 1 FROM n WHERE i < 2000) "
                           "INSERT INTO t SELECT printf('%0200d', i) FROM n;",
                           nullptr, nullptr, nullptr));
    sqlite3_close(db);
  }
  Immutable contents;
  ASSERT_TRUE(base::File::Read(path, &contents));
  ASSERT_LT(20 * 4096u, contents.size());
  String damaged = contents.string_copy();
  damaged.replace(10 * 4096, 4096, String(4096, '\xff'));
  ASSERT_TRUE(base::File::Write(path, Immutable(String(damaged))));

  // The quick start: the database stays.
  bool created = true;
  sqlite3_close(OpenDatabase(path, &created, false));
  EXPECT_FALSE(created);

  // E.g. after a crash.
  sqlite3_close(OpenDatabase(path, &created, true));
  EXPECT_TRUE(created);

  // Not a database at all - recreated even without the full check.
  ASSERT_TRUE(base::File::Write(path, Immutable(String(8192, 'x'))));
  sqlite3_close(OpenDatabase(path, &created, false));
  EXPECT_TRUE(created);
}

}  // namespace cache
}  // namespace dist_clang
