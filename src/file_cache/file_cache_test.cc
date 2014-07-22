#include <file_cache/file_cache.h>

#include <base/file_utils.h>
#include <base/future.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/public/gtest/gtest.h>
#include <third_party/libcxx/exported/include/regex>

namespace dist_clang {
namespace file_cache {

TEST(FileCacheTest, HashCompliesWithRegex) {
  const base::TemporaryDir tmp_dir;
  FileCache cache(tmp_dir);

  std::regex hash_regex("[a-z0-9]{32}-[a-z0-9]{8}-[a-z0-9]{8}");
  EXPECT_TRUE(std::regex_match(cache.Hash("1", "2", "3"), hash_regex));
}

TEST(FileCacheTest, LockNonExistentFile) {
  const base::TemporaryDir tmp_dir;
  const String absent_path = String(tmp_dir) + "/absent_file";
  FileCache cache(tmp_dir);

  {
    FileCache::ReadLock lock(&cache, absent_path);
    EXPECT_FALSE(lock);
  }
  {
    FileCache::WriteLock lock(&cache, absent_path);
    EXPECT_TRUE(lock);
  }
}

TEST(FileCacheTest, DoubleLocks) {
  const base::TemporaryDir tmp_dir;
  const String file_path = String(tmp_dir) + "/file";
  FileCache cache(tmp_dir);

  ASSERT_TRUE(base::WriteFile(file_path, "1"));
  {
    FileCache::ReadLock lock1(&cache, file_path);
    ASSERT_TRUE(lock1);
    {
      FileCache::ReadLock lock2(&cache, file_path);
      EXPECT_TRUE(lock2);
      FileCache::WriteLock lock3(&cache, file_path);
      EXPECT_FALSE(lock3);
    }
    FileCache::WriteLock lock4(&cache, file_path);
    EXPECT_FALSE(lock4);
  }
  {
    FileCache::WriteLock lock1(&cache, file_path);
    EXPECT_TRUE(lock1);
    FileCache::WriteLock lock2(&cache, file_path);
    EXPECT_FALSE(lock2);
    FileCache::ReadLock lock3(&cache, file_path);
    EXPECT_FALSE(lock3);
  }
  {
    FileCache::ReadLock lock1(&cache, file_path);
    EXPECT_TRUE(lock1);
  }
}

TEST(FileCacheTest, RemoveEntry) {
  const base::TemporaryDir tmp_dir;
  const String manifest_path = String(tmp_dir) + "/123.manifest";
  const String object_path = String(tmp_dir) + "/123.o";
  const String deps_path = String(tmp_dir) + "/123.d";
  const String stderr_path = String(tmp_dir) + "/123.stderr";

  {
    ASSERT_TRUE(base::WriteFile(manifest_path, "1"));
    ASSERT_TRUE(base::WriteFile(object_path, "1"));
    ASSERT_TRUE(base::WriteFile(deps_path, "1"));
    ASSERT_TRUE(base::WriteFile(stderr_path, "1"));
    FileCache cache(tmp_dir, 100);
    EXPECT_TRUE(cache.RemoveEntry(manifest_path));
    ASSERT_EQ(0u, base::CalculateDirectorySize(tmp_dir));
    EXPECT_EQ(0u, cache.cached_size_);
  }

  {
    ASSERT_TRUE(base::WriteFile(manifest_path, "1"));
    ASSERT_TRUE(base::WriteFile(object_path, "1"));
    FileCache cache(tmp_dir, 100);
    EXPECT_TRUE(cache.RemoveEntry(manifest_path));
    ASSERT_EQ(0u, base::CalculateDirectorySize(tmp_dir));
    EXPECT_EQ(0u, cache.cached_size_);
  }

  // TODO: check that |RemoveEntry()| fails, if at least one file can't be
  //       removed.
}

TEST(FileCacheTest, RestoreSingleEntry) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));

  // Check that entrie's content is not changed on cache miss.
  FileCache::Entry entry{object_path, deps_path, stderror};
  EXPECT_FALSE(cache.Find(code, cl, version, &entry));
  ASSERT_EQ(object_path, entry.object.GetPath());
  EXPECT_EQ(deps_path, entry.deps.GetPath());
  EXPECT_EQ(stderror, entry.stderr);

  // Store the entry.
  auto future = cache.Store(code, cl, version, entry);
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Restore the entry.
  ASSERT_TRUE(cache.Find(code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.object, &object_code));
  EXPECT_TRUE(base::ReadFile(entry.deps, &deps));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(expected_deps, deps);
  EXPECT_EQ(stderror, entry.stderr);
}

TEST(FileCacheTest, RestoreSingleEntry_Sync) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));

  FileCache::Entry entry{object_path, deps_path, stderror};
  EXPECT_FALSE(cache.Find(code, cl, version, &entry));
  ASSERT_EQ(object_path, entry.object.GetPath());
  EXPECT_EQ(deps_path, entry.deps.GetPath());
  EXPECT_EQ(stderror, entry.stderr);
  cache.StoreNow(code, cl, version, entry);
  ASSERT_TRUE(cache.Find(code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.object, &object_code));
  EXPECT_TRUE(base::ReadFile(entry.deps, &deps));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(expected_deps, deps);
  EXPECT_EQ(stderror, entry.stderr);
}

TEST(FileCacheTest, DISABLED_RestoreEntryWithMissingFile) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_RestoreEntryWithMalfordedManifest) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_RestoreNonExistentEntry) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_RestoreEntryLockedForReading) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_RestoreEntryLockedForWriting) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_StoreEntryLockedForWriting) {
  // TODO: implement this test.
  // This test is usefull for the sync mode, when multiple threads may call
  // |StoreNow()| simultaneously.
}

TEST(FileCacheTest, DISABLED_CleanEntryLockedForReading) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_CleanEntryLockedForWriting) {
  // TODO: implement this test.
}

TEST(FileCacheTest, DISABLED_BadInitialCacheSize) {
  // TODO: implement this test.
  //  - Check that max size becomes unlimited, if we can't calculate initial
  //    cache directory size.
}

TEST(FileCacheTest, ExceedCacheSize) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String cache_path = path + "/cache";
  const String obj_path[] = {path + "/obj1.o", path + "/obj2.o",
                             path + "/obj3.o"};
  const String obj_content[] = {"22", "333", "4444"};
  const String code[] = {"int main() { return 0; }", "int main() { return 1; }",
                         "int main() { return 2; }"};
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(base::WriteFile(obj_path[i], obj_content[i]));
  }

  FileCache cache(cache_path, 30);

  {
    FileCache::Entry entry{obj_path[0], String(), String()};
    auto future = cache.Store(code[0], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(14u, base::CalculateDirectorySize(cache_path));
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry{obj_path[1], String(), String()};
    auto future = cache.Store(code[1], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(29u, base::CalculateDirectorySize(cache_path));
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry{obj_path[2], String(), String()};
    auto future = cache.Store(code[2], cl, version, entry);
    ASSERT_TRUE(!!future);
    future->Wait();
    ASSERT_TRUE(future->GetValue());
    EXPECT_EQ(16u, base::CalculateDirectorySize(cache_path));
  }

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(code[0], cl, version, &entry));
  EXPECT_FALSE(cache.Find(code[1], cl, version, &entry));
  EXPECT_TRUE(cache.Find(code[2], cl, version, &entry));
}

TEST(FileCacheTest, DISABLED_DeleteOriginals) {
  // TODO: implement this test.
  //  - Check that original object and deps get deleted, if |entry.move_*| is
  //    true.
}

TEST(FileCacheTest, RestoreDirectEntry) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));
  ASSERT_TRUE(base::WriteFile(header1_path, "#define A"));
  ASSERT_TRUE(base::WriteFile(header2_path, "#define B"));

  // Store the entry.
  FileCache::Entry entry{object_path, deps_path, stderror};
  auto future = cache.Store(code, cl, version, entry);
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Store the direct entry.
  const String orig_code = "int main() {}";
  const List<String> headers = {header1_path, header2_path};
  future = cache.Store_Direct(orig_code, cl, version, headers,
                              FileCache::Hash(code, cl, version));
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Restore the entry.
  ASSERT_TRUE(cache.Find_Direct(orig_code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.object, &object_code));
  EXPECT_TRUE(base::ReadFile(entry.deps, &deps));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(expected_deps, deps);
  EXPECT_EQ(stderror, entry.stderr);
}

TEST(FileCacheTest, RestoreDirectEntry_Sync) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));
  ASSERT_TRUE(base::WriteFile(header1_path, "#define A"));
  ASSERT_TRUE(base::WriteFile(header2_path, "#define B"));

  // Store the entry.
  FileCache::Entry entry{object_path, deps_path, stderror};
  cache.StoreNow(code, cl, version, entry);

  // Store the direct entry.
  const String orig_code = "int main() {}";
  const List<String> headers = {header1_path, header2_path};
  cache.StoreNow_Direct(orig_code, cl, version, headers,
                        FileCache::Hash(code, cl, version));

  // Restore the entry.
  ASSERT_TRUE(cache.Find_Direct(orig_code, cl, version, &entry));
  ASSERT_TRUE(base::ReadFile(entry.object, &object_code));
  EXPECT_TRUE(base::ReadFile(entry.deps, &deps));
  EXPECT_EQ(expected_object_code, object_code);
  EXPECT_EQ(expected_deps, deps);
  EXPECT_EQ(stderror, entry.stderr);
}

TEST(FileCacheTest, DirectEntry_ChangedHeader) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  FileCache::Entry entry{object_path, deps_path, stderror};
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));
  ASSERT_TRUE(base::WriteFile(header1_path, "#define A"));
  ASSERT_TRUE(base::WriteFile(header2_path, "#define B"));

  // Store the entry.
  auto future = cache.Store(code, cl, version, entry);
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Store the direct entry.
  const String orig_code = "int main() {}";
  const List<String> headers = {header1_path, header2_path};
  future = cache.Store_Direct(orig_code, cl, version, headers,
                              FileCache::Hash(code, cl, version));
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Change header contents.
  ASSERT_TRUE(base::WriteFile(header1_path, "#define C"));

  // Restore the entry.
  EXPECT_FALSE(cache.Find_Direct(orig_code, cl, version, &entry));
}

TEST(FileCacheTest, DirectEntry_ChangedOriginalCode) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String stderror = "some warning";
  const String expected_object_code = "some object code";
  const String expected_deps = "some deps";
  FileCache cache(path);
  FileCache::Entry entry{object_path, deps_path, stderror};
  String object_code, deps;

  const String code = "int main() { return 0; }";
  const String cl = "-c";
  const String version = "3.5 (revision 100000)";

  ASSERT_TRUE(base::WriteFile(object_path, expected_object_code));
  ASSERT_TRUE(base::WriteFile(deps_path, expected_deps));
  ASSERT_TRUE(base::WriteFile(header1_path, "#define A"));
  ASSERT_TRUE(base::WriteFile(header2_path, "#define B"));

  // Store the entry.
  auto future = cache.Store(code, cl, version, entry);
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Store the direct entry.
  const String orig_code = "int main() {}";
  const List<String> headers = {header1_path, header2_path};
  future = cache.Store_Direct(orig_code, cl, version, headers,
                              FileCache::Hash(code, cl, version));
  ASSERT_TRUE(!!future);
  future->Wait();
  ASSERT_TRUE(future->GetValue());

  // Restore the entry.
  EXPECT_FALSE(cache.Find_Direct(orig_code + " ", cl, version, &entry));
}

}  // namespace file_cache
}  // namespace dist_clang
