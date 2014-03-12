#include "daemon/file_cache.h"

#include "base/file_utils.h"
#include "base/future_impl.h"
#include "base/temporary_dir.h"
#include "gtest/gtest.h"

namespace dist_clang {
namespace daemon {

TEST(FileCacheTest, RestoreSingleEntry) {
  const base::TemporaryDir tmp_dir;
  const std::string path = tmp_dir;
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
  auto future = cache.Store(code, cl, version, entry);
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());
  ASSERT_TRUE(cache.Find(code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.first, &object_code));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(stderror, entry.second);
}

TEST(FileCacheTest, ExceedCacheSize) {
  const base::TemporaryDir tmp_dir;
  const std::string path = tmp_dir;
  const std::string cache_path = path + "/cache";
  const std::string obj_path[] = {path + "/obj1.o", path + "/obj2.o",
                                  path + "/obj3.o"};
  const std::string obj_content[] = {"22", "22", "4444"};
  const std::string code[] = {"int main() { return 0; }",
                              "int main() { return 1; }",
                              "int main() { return 2; }"};
  const std::string cl = "-c";
  const std::string version = "3.5 (revision 100000)";
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(base::WriteFile(obj_path[i], obj_content[i]));
  }

  FileCache cache(cache_path, 5);

  {
    FileCache::Entry entry(obj_path[0], std::string());
    auto future = cache.Store(code[0], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(2u, base::CalculateDirectorySize(cache_path));
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry(obj_path[1], std::string());
    auto future = cache.Store(code[1], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(4u, base::CalculateDirectorySize(cache_path));
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry(obj_path[2], std::string());
    auto future = cache.Store(code[2], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(4u, base::CalculateDirectorySize(cache_path));
  }

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(code[0], cl, version, &entry));
  EXPECT_FALSE(cache.Find(code[1], cl, version, &entry));
  EXPECT_TRUE(cache.Find(code[2], cl, version, &entry));
}

}  // namespace daemon
}  // namespace dist_clang
