#include "daemon/file_cache.h"

#include "base/c_utils.h"
#include "base/file_utils.h"
#include "gtest/gtest.h"

namespace dist_clang {
namespace daemon {

TEST(FileCacheTest, RestoreSingleEntry) {
  const std::string path = base::CreateTempDir();
  const std::string entry_path = path + "/test.o";
  const std::string stderror = "some warning";
  const std::string expected_object_code = "some object code";
  FileCache cache(path);
  FileCache::Entry entry(entry_path, stderror);
  std::string object_code;

  const std::string code = "int main() { return 0; }";
  const std::string cl = "-c";
  const std::string version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(entry_path, expected_object_code));
  EXPECT_FALSE(cache.Find(code, cl, version, &entry));
  ASSERT_EQ(entry_path, entry.first);
  EXPECT_EQ(stderror, entry.second);
  cache.Store(code, cl, version, entry);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_TRUE(cache.Find(code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.first, &object_code));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(stderror, entry.second);
}

}  // namespace daemon
}  // namespace dist_clang
