#include <cache/database_sqlite.h>

#include <base/temporary_dir.h>

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

}  // namespace cache
}  // namespace dist_clang
