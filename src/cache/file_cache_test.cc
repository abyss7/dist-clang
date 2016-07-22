#include <cache/file_cache.h>

#include <base/file/file.h>
#include <base/future.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>
#include STL(regex)

namespace dist_clang {
namespace cache {

using namespace string;

TEST(FileCacheTest, HashCompliesWithRegex) {
  std::regex hash_regex("[a-f0-9]{32}-[a-f0-9]{8}-[a-f0-9]{8}");
  EXPECT_TRUE(std::regex_match(
      FileCache::Hash(HandledSource("1"_l), CommandLine("2"_l), Version("3"_l))
          .str.string_copy(),
      hash_regex));
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

  ASSERT_TRUE(base::File::Write(file_path, "1"_l));
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
  FileCache cache(tmp_dir, 100, false, false);

  string::Hash hash1{"12345678901234567890123456789012-12345678-00000001"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash1)));
    const String common_path = cache.CommonPath(hash1);
    const String manifest_path = common_path + ".manifest";
    const String object_path = common_path + ".o";
    const String deps_path = common_path + ".d";
    const String stderr_path = common_path + ".stderr";

    proto::Manifest manifest;
    manifest.set_version(1);
    manifest.mutable_v1()->set_obj(true);
    manifest.mutable_v1()->set_dep(true);
    manifest.mutable_v1()->set_err(true);
    ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
    ASSERT_TRUE(base::File::Write(object_path, "1"_l));
    ASSERT_TRUE(base::File::Write(deps_path, "1"_l));
    ASSERT_TRUE(base::File::Write(stderr_path, "1"_l));
  }

  string::Hash hash2{"12345678901234567890123456789012-12345678-00000002"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash2)));
    const String common_path = cache.CommonPath(hash2);
    const String manifest_path = common_path + ".manifest";
    const String object_path = common_path + ".o";
    const String deps_path = common_path + ".d";

    proto::Manifest manifest;
    manifest.set_version(1);
    manifest.mutable_v1()->set_obj(true);
    manifest.mutable_v1()->set_dep(true);
    manifest.mutable_v1()->set_err(true);
    ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
    ASSERT_TRUE(base::File::Write(object_path, "1"_l));
    ASSERT_TRUE(base::File::Write(deps_path, "1"_l));
  }

  string::Hash hash3{"12345678901234567890123456789012-12345678-00000003"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash3)));
    const String common_path = cache.CommonPath(hash3);
    const String manifest_path = common_path + ".manifest";
    const String object_path = common_path + ".o";
    const String deps_path = common_path + ".d";

    ASSERT_TRUE(base::File::Write(manifest_path, "1"_l));
    ASSERT_TRUE(base::File::Write(object_path, "1"_l));
    ASSERT_TRUE(base::File::Write(deps_path, "1"_l));
  }

  ASSERT_TRUE(cache.Run(1));
  EXPECT_TRUE(cache.RemoveEntry(hash1));
  EXPECT_TRUE(cache.RemoveEntry(hash2));
  EXPECT_FALSE(cache.RemoveEntry(hash3));
  // If we can't even read manifest, then the entry should be removed when we
  // try to migrate it.

  auto db_size = cache.database_->SizeOnDisk();
  EXPECT_EQ(cache.cache_size_, base::CalculateDirectorySize(tmp_dir) - db_size);
  EXPECT_FALSE(cache.entries_->Exists(hash1.str));
  EXPECT_FALSE(cache.entries_->Exists(hash2.str));
  EXPECT_FALSE(cache.entries_->Exists(hash3.str));
}

TEST(FileCacheTest, RestoreSingleEntry) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  EXPECT_FALSE(cache.Find(code, cl, version, &entry1));
  EXPECT_TRUE(entry1.object.empty());
  EXPECT_TRUE(entry1.deps.empty());
  EXPECT_TRUE(entry1.stderr.empty());

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  cache.Store(code, cl, version, entry1);

  ASSERT_TRUE(cache.Find(code, cl, version, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, RestoreEntryWithMissingFile) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(code, cl, version, entry1);

  base::File::Delete(cache.CommonPath(FileCache::Hash(code, cl, version)) +
                     ".d");

  // Restore the entry.
  ASSERT_FALSE(cache.Find(code, cl, version, &entry2));
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
  // |Store()| simultaneously.
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
  const Literal obj_content[] = {"22"_l, "333"_l, "4444"_l};
  const HandledSource code[] = {HandledSource("int main() { return 0; }"_l),
                                HandledSource("int main() { return 1; }"_l),
                                HandledSource("int main() { return 2; }"_l)};
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  FileCache cache(cache_path, 138, false, false);
  // 138 = sizeof(obj_content[0]) + sizeof(obj_content[1]) + 1 + 2 *
  // <size_of_manifest>. The current typical size of manifest is 66 bytes.

  ASSERT_TRUE(cache.Run(1));
  auto db_size = cache.database_->SizeOnDisk();

  {
    FileCache::Entry entry{obj_content[0], String(), String()};
    cache.Store(code[0], cl, version, entry);
    EXPECT_EQ(68u, base::CalculateDirectorySize(cache_path) - db_size);
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry{obj_content[1], String(), String()};
    cache.Store(code[1], cl, version, entry);
    EXPECT_EQ(137u, base::CalculateDirectorySize(cache_path) - db_size);
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  {
    FileCache::Entry entry{obj_content[2], String(), String()};
    cache.Store(code[2], cl, version, entry);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_EQ(70u, base::CalculateDirectorySize(cache_path) - db_size);
  }

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(code[0], cl, version, &entry));
  EXPECT_FALSE(cache.Find(code[1], cl, version, &entry));
  EXPECT_TRUE(cache.Find(code[2], cl, version, &entry));
}

TEST(FileCacheTest, RestoreDirectEntry) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String source_path = path + "/test.c";
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String header2_rel_path = "test2.h";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "test.o: test.c /test1.h test2.h"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  const UnhandledSource orig_code("int main() {}"_l);
  ASSERT_TRUE(base::File::Write(source_path, orig_code));
  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(code, cl, version, entry1);

  // Store the direct entry.
  const List<String> headers = {source_path, header1_path, header2_rel_path};
  cache.Store(orig_code, cl, version, headers, path,
              FileCache::Hash(code, cl, version));

  // Restore the entry.
  ASSERT_TRUE(cache.Find(orig_code, cl, version, source_path, path, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, DirectEntry_ChangedHeaderContents) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String source_path = path + "/test.c";
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const String header2_rel_path = "test2.h";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  const UnhandledSource orig_code("int main() {}"_l);
  ASSERT_TRUE(base::File::Write(source_path, orig_code));
  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(code, cl, version, entry1);

  // Store the direct entry.
  const List<String> headers = {source_path, header1_path, header2_rel_path};
  cache.Store(orig_code, cl, version, headers, path,
              FileCache::Hash(code, cl, version));

  // Check that entry can be restored.
  EXPECT_TRUE(cache.Find(orig_code, cl, version, source_path, path, &entry2));

  // Change header contents.
  ASSERT_TRUE(base::File::Write(header2_path, "#define C"_l));

  // Restore the entry.
  EXPECT_FALSE(cache.Find(orig_code, cl, version, source_path, path, &entry2));
}

TEST(FileCacheTest, DirectEntry_RewriteManifest) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String source_path = path + "/test.c";
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";

  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  const UnhandledSource orig_code("int main() {}"_l);
  ASSERT_TRUE(base::File::Write(source_path, orig_code));
  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(code, cl, version, entry1);

  // Store the direct entry.
  List<String> headers = {source_path, header1_path, header2_path};
  cache.Store(orig_code, cl, version, headers, path,
              FileCache::Hash(code, cl, version));

  headers.pop_back();

  // Store the direct entry - again.
  cache.Store(orig_code, cl, version, headers, path,
              FileCache::Hash(code, cl, version));

  // Restore the entry.
  EXPECT_TRUE(cache.Find(orig_code, cl, version, source_path, path, &entry2));
}

TEST(FileCacheTest, DirectEntry_ChangedOriginalCode) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const String header1_path = path + "/test1.h";
  const String header2_path = path + "/test2.h";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(path);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry.object = expected_object_code;
  entry.deps = expected_deps;
  entry.stderr = expected_stderr;

  // Store the entry.
  cache.Store(code, cl, version, entry);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_path};
  cache.Store(orig_code, cl, version, headers, path,
              FileCache::Hash(code, cl, version));

  // Restore the entry.
  const UnhandledSource bad_orig_code(orig_code.str.string_copy() + " ");
  EXPECT_FALSE(cache.Find(bad_orig_code, cl, version, "", path, &entry));
}

TEST(FileCacheTest, RestoreAndMigrateSnappyEntry) {
  const base::TemporaryDir tmp_dir;
  const String path = tmp_dir;
  const String object_path = path + "/test.o";
  const String deps_path = path + "/test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;

  {
    FileCache cache(path, 1000, true, false);
    ASSERT_TRUE(cache.Run(1));
    FileCache::Entry entry1, entry2;

    const HandledSource code("int main() { return 0; }"_l);
    const CommandLine cl("-c"_l);
    const Version version("3.5 (revision 100000)"_l);

    ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
    ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
    EXPECT_FALSE(cache.Find(code, cl, version, &entry1));
    EXPECT_TRUE(entry1.object.empty());
    EXPECT_TRUE(entry1.deps.empty());
    EXPECT_TRUE(entry1.stderr.empty());

    entry1.object = expected_object_code;
    entry1.deps = expected_deps;
    entry1.stderr = expected_stderr;

    cache.Store(code, cl, version, entry1);

    ASSERT_TRUE(cache.Find(code, cl, version, &entry2));
    EXPECT_EQ(expected_object_code, entry2.object);
    EXPECT_EQ(expected_deps, entry2.deps);
    EXPECT_EQ(expected_stderr, entry2.stderr);
  }
  {
    FileCache cache(path, 1000, true, false);
    ASSERT_TRUE(cache.Run(1));
    FileCache::Entry entry;
    const HandledSource code("int main() { return 0; }"_l);
    const CommandLine cl("-c"_l);
    const Version version("3.5 (revision 100000)"_l);
    ASSERT_TRUE(cache.Find(code, cl, version, &entry));
    EXPECT_EQ(expected_object_code, entry.object);
    EXPECT_EQ(expected_deps, entry.deps);
    EXPECT_EQ(expected_stderr, entry.stderr);
  }
}

TEST(FileCacheTest, UseIndexFromDisk) {
  const base::TemporaryDir tmp_dir;
  string::Hash hash{"12345678901234567890123456789012-12345678-00000001"_l};

  {
    FileCache cache(tmp_dir, FileCache::UNLIMITED, false, true);
    const String manifest_path = cache.CommonPath(hash) + ".manifest";

    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));

    proto::Manifest manifest;
    manifest.set_stderr(false);
    manifest.set_object(false);
    manifest.set_deps(false);

    ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
    manifest.Clear();
    EXPECT_TRUE(cache.Run(1));
    ASSERT_TRUE(base::LoadFromFile(manifest_path, &manifest));
    EXPECT_EQ(FileCache::kManifestVersion, manifest.version());
  }

  {
    FileCache cache(tmp_dir, FileCache::UNLIMITED, false, true);
    const String manifest_path = cache.CommonPath(hash) + ".manifest";

    ASSERT_TRUE(base::File::Write(manifest_path, "1"_l));
    EXPECT_TRUE(cache.Run(1));
    EXPECT_TRUE(base::File::Exists(manifest_path));
  }
}

}  // namespace cache
}  // namespace dist_clang
