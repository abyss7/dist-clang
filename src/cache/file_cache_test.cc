#include <cache/file_cache.h>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/future.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>

#include <base/file_utils.h>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include STL(regex)

namespace dist_clang {
namespace cache {

using namespace string;

TEST(FileCacheTest, HashCompliesWithRegex) {
  std::regex hash_regex("[a-f0-9]{32}-[a-f0-9]{8}-[a-f0-9]{8}");
  EXPECT_TRUE(
      std::regex_match(FileCache::Hash(HandledSource("1"_l), {},
                                       CommandLine("3"_l), Version("4"_l))
                           .str.string_copy(),
                       hash_regex));
  EXPECT_TRUE(
      std::regex_match(FileCache::Hash(HandledSource("1"_l),
                                       ExtraFiles{{SANITIZE_BLACKLIST, "21"_l}},
                                       CommandLine("3"_l), Version("4"_l))
                           .str.string_copy(),
                       hash_regex));
}

TEST(FileCacheTest, ExtraFilesGiveDifferentHash) {
  String hash_wo_extra_file =
      FileCache::Hash(HandledSource("1"_l), {}, CommandLine("3"_l),
                      Version("4"_l))
          .str.string_copy();
  String hash_with_extra_file =
      FileCache::Hash(HandledSource("1"_l),
                      ExtraFiles{{SANITIZE_BLACKLIST, "2"_l}},
                      CommandLine("3"_l), Version("4"_l))
          .str.string_copy();
  EXPECT_NE(hash_wo_extra_file, hash_with_extra_file);
}

TEST(FileCacheTest, LockNonExistentFile) {
  const base::TemporaryDir temp_dir;
  const auto absent_path = temp_dir.path() / "absent_file";
  FileCache cache(temp_dir);

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
  const base::TemporaryDir temp_dir;
  const auto file_path = temp_dir.path() / "file";
  FileCache cache(temp_dir);

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
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, 100, false, false);

  string::Hash hash1{"12345678901234567890123456789012" "-12345678-00000001"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash1)));
    const auto common_prefix = cache.CommonPath(hash1);
    const auto manifest_path =
        AppendExtension(common_prefix, base::kExtManifest);
    const auto object_path = AppendExtension(common_prefix, base::kExtObject);
    const auto deps_path = AppendExtension(common_prefix, base::kExtDeps);
    const auto stderr_path = AppendExtension(common_prefix, base::kExtStderr);

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

  string::Hash hash2{"12345678901234567890123456789012" "-12345678-00000002"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash2)));
    const auto common_prefix = cache.CommonPath(hash2);
    const auto manifest_path =
        AppendExtension(common_prefix, base::kExtManifest);
    const auto object_path = AppendExtension(common_prefix, base::kExtObject);
    const auto deps_path = AppendExtension(common_prefix, base::kExtDeps);

    proto::Manifest manifest;
    manifest.set_version(1);
    manifest.mutable_v1()->set_obj(true);
    manifest.mutable_v1()->set_dep(true);
    manifest.mutable_v1()->set_err(true);
    ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));
    ASSERT_TRUE(base::File::Write(object_path, "1"_l));
    ASSERT_TRUE(base::File::Write(deps_path, "1"_l));
  }

  string::Hash hash3{"12345678901234567890123456789012" "-12345678-00000003"_l};
  {
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash3)));
    const auto common_prefix = cache.CommonPath(hash3);
    const auto manifest_path =
        AppendExtension(common_prefix, base::kExtManifest);
    const auto object_path = AppendExtension(common_prefix, base::kExtObject);
    const auto deps_path = AppendExtension(common_prefix, base::kExtDeps);

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
  EXPECT_EQ(cache.cache_size_,
            base::CalculateDirectorySize(temp_dir) - db_size);
  EXPECT_FALSE(cache.entries_->Exists(hash1.str));
  EXPECT_FALSE(cache.entries_->Exists(hash2.str));
  EXPECT_FALSE(cache.entries_->Exists(hash3.str));
}

TEST(FileCacheTest, RestoreSingleEntry) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  EXPECT_FALSE(cache.Find(hash, &entry1));
  EXPECT_TRUE(entry1.object.empty());
  EXPECT_TRUE(entry1.deps.empty());
  EXPECT_TRUE(entry1.stderr.empty());

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  cache.Store(hash, entry1);

  ASSERT_TRUE(cache.Find(hash, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, RestoreSingleEntryWithExtraFile) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const Immutable extra_file("fun:main"_l);
  const Immutable another_extra_file("fun:main "_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash1 = FileCache::Hash(
      code, ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl, version);
  const auto hash2 = FileCache::Hash(
      code, ExtraFiles{{SANITIZE_BLACKLIST, another_extra_file}}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  EXPECT_FALSE(cache.Find(hash1, &entry1));
  EXPECT_TRUE(entry1.object.empty());
  EXPECT_TRUE(entry1.deps.empty());
  EXPECT_TRUE(entry1.stderr.empty());

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  cache.Store(hash1, entry1);

  EXPECT_FALSE(cache.Find(FileCache::Hash(code, {}, cl, version), &entry2));
  EXPECT_FALSE(cache.Find(hash2, &entry2));
  ASSERT_TRUE(cache.Find(hash1, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, RoundTripCompressedEntry) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, true, false);
  ASSERT_TRUE(cache.Run(1));

  // Object files have long runs of zeros and repeated patterns - they used to
  // break an old version of Snappy.
  String object(1 << 20, '\0'), dwo;
  for (size_t i = 0; i < object.size(); i += 97) {
    object[i] = static_cast<char>(i % 251);
  }
  for (size_t i = 0; i < 100000; ++i) {
    dwo.push_back("\x7f" "ELF\0\x11\x01"[i % 7]);
  }

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  cache.Store(hash, FileCache::Entry{Immutable(String(object)),
                                     Immutable(String("deps")),
                                     Immutable(String("stderr")),
                                     Immutable(String(dwo))});

  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  EXPECT_EQ(object, entry.object.string_copy());
  EXPECT_EQ(dwo, entry.dwo.string_copy());
  EXPECT_EQ("deps", entry.deps.string_copy());
  EXPECT_EQ("stderr", entry.stderr.string_copy());
}

TEST(FileCacheTest, LinkOutputs) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto dwo_path = temp_dir.path() / "test.dwo";
  const auto restored_path = temp_dir.path() / "restored.o";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, true, false,
                  true);
  ASSERT_TRUE(cache.Run(1));

  ASSERT_TRUE(base::File::Write(object_path, "object"_l));
  ASSERT_TRUE(base::File::Write(dwo_path, "dwo"_l));

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  FileCache::Entry stored;
  stored.deps = "deps"_l;
  stored.object_path = object_path;
  stored.dwo_path = dwo_path;
  cache.Store(hash, stored);

  // The outputs are linked, not read.
  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  EXPECT_TRUE(entry.object.empty());
  EXPECT_TRUE(entry.dwo.empty());
  EXPECT_EQ("deps"_l, entry.deps);
  ASSERT_FALSE(entry.object_path.empty());
  ASSERT_FALSE(entry.dwo_path.empty());

  Immutable object, dwo;
  ASSERT_TRUE(base::File::Read(entry.object_path, &object));
  ASSERT_TRUE(base::File::Read(entry.dwo_path, &dwo));
  EXPECT_EQ("object"_l, object);
  EXPECT_EQ("dwo"_l, dwo);

  // The cache files stay writable: compilers refuse to replace read-only
  // outputs.
  struct stat st;
  ASSERT_EQ(0, stat(entry.object_path.c_str(), &st));
  EXPECT_NE(0u, st.st_mode & S_IWUSR);

  // A shared file, modified in place, is detected - even after the link is
  // removed.
  base::File::CloneMode mode;
  ASSERT_TRUE(base::File::Clone(entry.object_path, restored_path, true, &mode));
  if (mode == base::File::CloneMode::HARDLINK) {
    FILE* file = fopen(restored_path.c_str(), "r+");
    ASSERT_TRUE(file);
    fputs("OBJECT", file);
    fclose(file);
    ASSERT_TRUE(base::File::Delete(restored_path));

    FileCache::Entry broken_entry;
    EXPECT_FALSE(cache.Find(hash, &broken_entry));
  }
}

TEST(FileCacheTest, LinkOutputsWithContents) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, false,
                  false, true);
  ASSERT_TRUE(cache.Run(1));

  // E.g. the result of the remote compilation - there is no file to link.
  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, Immutable()});

  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  ASSERT_FALSE(entry.object_path.empty());
  Immutable object;
  ASSERT_TRUE(base::File::Read(entry.object_path, &object));
  EXPECT_EQ("object"_l, object);
}

TEST(FileCacheTest, RestoreEntryWithMissingFile) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry1);

  base::File::Delete(AppendExtension(cache.CommonPath(hash), "d"_l));

  // Restore the entry.
  ASSERT_FALSE(cache.Find(hash, &entry2));
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
  const base::TemporaryDir temp_dir;
  const auto cache_path = temp_dir.path() / "cache";
  const Literal obj_content[] = {"22"_l, "333"_l, "4444"_l};
  const HandledSource code[] = {HandledSource("int main() { return 0; }"_l),
                                HandledSource("int main() { return 1; }"_l),
                                HandledSource("int main() { return 2; }"_l)};
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);

  // Measure the sizes of entries on disk - they depend on the manifest format.
  ui64 entry_size[3];
  {
    const base::TemporaryDir probe_dir;
    FileCache probe(probe_dir.path() / "cache", FileCache::UNLIMITED, false,
                    false);
    ASSERT_TRUE(probe.Run(1));
    const auto probe_db_size = probe.database_->SizeOnDisk();
    ui64 total_size = 0;
    for (int i = 0; i < 3; ++i) {
      probe.Store(FileCache::Hash(code[i], {}, cl, version),
                  FileCache::Entry{obj_content[i], String(), String()});
      const auto new_total_size =
          base::CalculateDirectorySize(probe_dir.path() / "cache") -
          probe_db_size;
      entry_size[i] = new_total_size - total_size;
      total_size = new_total_size;
    }
  }

  // Two first entries fit, but not with the third one.
  FileCache cache(cache_path, entry_size[0] + entry_size[1], false, false);

  ASSERT_TRUE(cache.Run(1));
  auto db_size = cache.database_->SizeOnDisk();

  {
    FileCache::Entry entry{obj_content[0], String(), String()};
    cache.Store(FileCache::Hash(code[0], {}, cl, version), entry);
    EXPECT_EQ(entry_size[0],
              base::CalculateDirectorySize(cache_path) - db_size);
  }

  std::this_thread::sleep_for(Seconds(1));

  {
    FileCache::Entry entry{obj_content[1], String(), String()};
    cache.Store(FileCache::Hash(code[1], {}, cl, version), entry);
    EXPECT_EQ(entry_size[0] + entry_size[1],
              base::CalculateDirectorySize(cache_path) - db_size);
  }

  std::this_thread::sleep_for(Seconds(1));

  {
    FileCache::Entry entry{obj_content[2], String(), String()};
    cache.Store(FileCache::Hash(code[2], {}, cl, version), entry);
    std::this_thread::sleep_for(Seconds(3));
    EXPECT_EQ(entry_size[2],
              base::CalculateDirectorySize(cache_path) - db_size);
  }

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(FileCache::Hash(code[0], {}, cl, version), &entry));
  EXPECT_FALSE(cache.Find(FileCache::Hash(code[1], {}, cl, version), &entry));
  EXPECT_TRUE(cache.Find(FileCache::Hash(code[2], {}, cl, version), &entry));
}

TEST(FileCacheTest, EvictNeverHitEntriesFirst) {
  const base::TemporaryDir temp_dir;
  const auto cache_path = temp_dir.path() / "cache";
  const Literal obj_content[] = {"22"_l, "333"_l, "4444"_l};
  const HandledSource code[] = {HandledSource("int main() { return 0; }"_l),
                                HandledSource("int main() { return 1; }"_l),
                                HandledSource("int main() { return 2; }"_l)};
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  auto Hash = [&](int i) { return FileCache::Hash(code[i], {}, cl, version); };

  ui64 total_size = 0;
  {
    const base::TemporaryDir probe_dir;
    FileCache probe(probe_dir.path() / "cache", FileCache::UNLIMITED, false,
                    false);
    ASSERT_TRUE(probe.Run(1));
    const auto probe_db_size = probe.database_->SizeOnDisk();
    for (int i = 0; i < 3; ++i) {
      probe.Store(Hash(i), FileCache::Entry{obj_content[i], String(), String()});
    }
    total_size = base::CalculateDirectorySize(probe_dir.path() / "cache") -
                 probe_db_size;
  }

  // Only one entry doesn't fit.
  FileCache cache(cache_path, total_size - 1, false, false);
  ASSERT_TRUE(cache.Run(1));

  // The first entry is used again - the second one is removed, though it's
  // newer.
  cache.Store(Hash(0), FileCache::Entry{obj_content[0], String(), String()});
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  cache.Store(Hash(1), FileCache::Entry{obj_content[1], String(), String()});
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  FileCache::Entry entry0;
  ASSERT_TRUE(cache.Find(Hash(0), &entry0));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  cache.Store(Hash(2), FileCache::Entry{obj_content[2], String(), String()});
  std::this_thread::sleep_for(Seconds(3));

  FileCache::Entry entry1, entry2, entry3;
  EXPECT_TRUE(cache.Find(Hash(0), &entry1));
  EXPECT_FALSE(cache.Find(Hash(1), &entry2));
  EXPECT_TRUE(cache.Find(Hash(2), &entry3));
}

TEST(FileCacheTest, KeepEntriesLinkedWithBuildDirs) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const HandledSource code[] = {HandledSource("int main() { return 0; }"_l),
                                HandledSource("int main() { return 1; }"_l),
                                HandledSource("int main() { return 2; }"_l)};
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  auto Hash = [&](int i) { return FileCache::Hash(code[i], {}, cl, version); };

  // All entries, that can be removed, should be removed: the disk is never
  // free enough.
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, false,
                  false, true);
  cache.SetMinFreePercent(100);
  ASSERT_TRUE(cache.Run(1));

  ASSERT_TRUE(base::File::Write(object_path, "object"_l));
  FileCache::Entry linked;
  linked.object_path = object_path;
  cache.Store(Hash(0), linked);

  struct stat st;
  ASSERT_EQ(0, stat(object_path.c_str(), &st));
  if (st.st_nlink < 2) {
    // The filesystem doesn't support hard links - nothing to check.
    return;
  }

  cache.Store(Hash(1), FileCache::Entry{"object1"_l, String(), String()});
  std::this_thread::sleep_for(Seconds(3));

  FileCache::Entry entry0, entry1;
  EXPECT_TRUE(cache.Find(Hash(0), &entry0));
  EXPECT_FALSE(cache.Find(Hash(1), &entry1));

  // The link is gone - the entry isn't protected anymore.
  ASSERT_TRUE(base::File::Delete(object_path));
  cache.Store(Hash(2), FileCache::Entry{"object2"_l, String(), String()});
  std::this_thread::sleep_for(Seconds(3));

  FileCache::Entry entry2, entry3;
  EXPECT_FALSE(cache.Find(Hash(0), &entry2));
  EXPECT_FALSE(cache.Find(Hash(2), &entry3));
}

TEST(FileCacheTest, RestoreDirectEntry) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto header2_rel_path = Path("test2.h");
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry1);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_rel_path};
  cache.Store(orig_code, {}, cl, version, headers, {}, temp_dir, hash);

  // Restore the entry.
  ASSERT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, RestoreDirectEntryWithExtraFile) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto header2_rel_path = Path("test2.h");
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const Immutable extra_file("fun:main"_l);
  const Immutable another_extra_file("fun:main "_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(
      code, ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry1);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_rel_path};
  cache.Store(orig_code, ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl,
              version, headers, {}, temp_dir, hash);

  // Restore the entry.
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry2));
  EXPECT_FALSE(cache.Find(orig_code,
                          ExtraFiles{{SANITIZE_BLACKLIST, another_extra_file}},
                          cl, version, temp_dir, &entry2));
  ASSERT_TRUE(cache.Find(orig_code,
                         ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl,
                         version, temp_dir, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
}

TEST(FileCacheTest, DirectEntry_SeveralHeaderSets) {
  const base::TemporaryDir temp_dir;
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("#include \"test.h\""_l);

  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  // The same source includes different headers, e.g. on different branches.
  const auto hash1 = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  cache.Store(hash1, FileCache::Entry{"object1"_l, "deps1"_l, Immutable()});
  cache.Store(orig_code, {}, cl, version, {header1_path}, {}, temp_dir, hash1);

  const auto hash2 = FileCache::Hash(HandledSource("2"_l), {}, cl, version);
  cache.Store(hash2, FileCache::Entry{"object2"_l, "deps2"_l, Immutable()});
  cache.Store(orig_code, {}, cl, version, {header2_path}, {}, temp_dir, hash2);

  FileCache::Entry entry1;
  ASSERT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry1));
  EXPECT_EQ("object2"_l, entry1.object);

  // The second set doesn't match anymore - the first one still does.
  ASSERT_TRUE(base::File::Write(header2_path, "#define C"_l));
  FileCache::Entry entry2;
  ASSERT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry2));
  EXPECT_EQ("object1"_l, entry2.object);

  ASSERT_TRUE(base::File::Write(header1_path, "#define C"_l));
  FileCache::Entry entry3;
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry3));
}

TEST(FileCacheTest, DirectEntry_LimitedHeaderSets) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("#include \"test.h\""_l);

  auto Store = [&](int i) {
    const auto header_path = temp_dir.path() / ("test" + std::to_string(i));
    ASSERT_TRUE(base::File::Write(header_path, Immutable(std::to_string(i))));
    const auto hash = FileCache::Hash(
        HandledSource(Immutable(std::to_string(i))), {}, cl, version);
    cache.Store(hash, FileCache::Entry{Immutable(std::to_string(i)), "deps"_l,
                                       Immutable()});
    cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash);
  };

  for (int i = 0; i <= int(FileCache::kMaxDirectVariants); ++i) {
    Store(i);
  }

  // Only the latest variant is present in all headers - remove the latest
  // ones one by one, and check the previous ones are still found, except the
  // oldest one.
  for (int i = FileCache::kMaxDirectVariants; i > 0; --i) {
    FileCache::Entry entry;
    ASSERT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
    EXPECT_EQ(std::to_string(i), entry.object.string_copy());
    ASSERT_TRUE(base::File::Delete(temp_dir.path() / ("test" + std::to_string(i))));
  }

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
}

TEST(FileCacheTest, ReplaceLastSource) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("1"_l);
  const auto hash1 = FileCache::Hash(UnhandledSource("1"_l), {}, cl, version);
  const auto hash2 = FileCache::Hash(UnhandledSource("2"_l), {}, cl, version);

  EXPECT_FALSE(cache.ReplaceLastSource("a.cc", hash1));
  EXPECT_FALSE(cache.ReplaceLastSource("a.cc", hash1));
  EXPECT_FALSE(cache.ReplaceLastSource("b.cc", hash2));
  EXPECT_TRUE(cache.ReplaceLastSource("a.cc", hash2));
  EXPECT_FALSE(cache.ReplaceLastSource("a.cc", hash2));
  EXPECT_TRUE(cache.ReplaceLastSource("a.cc", hash1));
}

TEST(FileCacheTest, DirectEntry_ChangedHeaderContents) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto header2_rel_path = Path("test2.h");
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry.object = expected_object_code;
  entry.deps = expected_deps;
  entry.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_rel_path};
  cache.Store(orig_code, {}, cl, version, headers, {}, temp_dir, hash);

  // Change header contents.
  ASSERT_TRUE(base::File::Write(header2_path, "#define C"_l));

  // Restore the entry.
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
}

TEST(FileCacheTest, DirectEntry_ChangedPreprocessedHeaderContents) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto preprocessed_header_path = temp_dir.path() / "precompile.h.pth";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));
  ASSERT_TRUE(
      base::File::Write(preprocessed_header_path, "Any content should work"_l));

  entry.object = expected_object_code;
  entry.deps = expected_deps;
  entry.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_path};
  const List<String> preprocessed_headers = {preprocessed_header_path};
  cache.Store(orig_code, {}, cl, version, headers, preprocessed_headers,
              temp_dir, hash);

  // Change header contents.
  ASSERT_TRUE(base::File::Write(preprocessed_header_path, "Other content"_l));

  // Restore the entry.
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
}

TEST(FileCacheTest, DirectEntry_RewriteManifest) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";

  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry1, entry2;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry1.object = expected_object_code;
  entry1.deps = expected_deps;
  entry1.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry1);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  List<String> headers = {header1_path, header2_path};
  cache.Store(orig_code, {}, cl, version, headers, {}, temp_dir, hash);

  headers.pop_back();

  // Store the direct entry - again.
  cache.Store(orig_code, {}, cl, version, headers, {}, temp_dir, hash);

  // Restore the entry.
  EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry2));
}

TEST(FileCacheTest, DirectEntry_ChangedOriginalCode) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry;

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry.object = expected_object_code;
  entry.deps = expected_deps;
  entry.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_path};
  cache.Store(orig_code, {}, cl, version, headers, {}, temp_dir, hash);

  // Restore the entry.
  const UnhandledSource bad_orig_code(orig_code.str.string_copy() + " ");
  EXPECT_FALSE(cache.Find(bad_orig_code, {}, cl, version, temp_dir, &entry));
}

TEST(FileCacheTest, DirectEntry_ChangedExtraFile) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto header1_path = temp_dir.path() / "test1.h";
  const auto header2_path = temp_dir.path() / "test2.h";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  FileCache::Entry entry;

  const HandledSource code("int main() { return 0; }"_l);
  const Immutable extra_file("fun:main"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(
      code, ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl, version);

  ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
  ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  entry.object = expected_object_code;
  entry.deps = expected_deps;
  entry.stderr = expected_stderr;

  // Store the entry.
  cache.Store(hash, entry);

  // Store the direct entry.
  const UnhandledSource orig_code("int main() {}"_l);
  const List<String> headers = {header1_path, header2_path};
  cache.Store(orig_code, ExtraFiles{{SANITIZE_BLACKLIST, extra_file}}, cl,
              version, headers, {}, temp_dir, hash);

  // Restore the entry.
  const Immutable new_extra_file("src:main.cc"_l);
  EXPECT_FALSE(cache.Find(orig_code,
                          ExtraFiles{{SANITIZE_BLACKLIST, new_extra_file}}, cl,
                          version, temp_dir, &entry));
}

TEST(FileCacheTest, RestoreAndMigrateSnappyEntry) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto expected_stderr = "some warning"_l;
  const auto expected_object_code = "some object code"_l;
  const auto expected_deps = "some deps"_l;

  {
    FileCache cache(temp_dir, 1000, true, false);
    ASSERT_TRUE(cache.Run(1));
    FileCache::Entry entry1, entry2;

    const HandledSource code("int main() { return 0; }"_l);
    const CommandLine cl("-c"_l);
    const Version version("3.5 (revision 100000)"_l);
    const auto hash = FileCache::Hash(code, {}, cl, version);

    ASSERT_TRUE(base::File::Write(object_path, expected_object_code));
    ASSERT_TRUE(base::File::Write(deps_path, expected_deps));
    EXPECT_FALSE(cache.Find(hash, &entry1));
    EXPECT_TRUE(entry1.object.empty());
    EXPECT_TRUE(entry1.deps.empty());
    EXPECT_TRUE(entry1.stderr.empty());

    entry1.object = expected_object_code;
    entry1.deps = expected_deps;
    entry1.stderr = expected_stderr;

    cache.Store(hash, entry1);

    ASSERT_TRUE(cache.Find(hash, &entry2));
    EXPECT_EQ(expected_object_code, entry2.object);
    EXPECT_EQ(expected_deps, entry2.deps);
    EXPECT_EQ(expected_stderr, entry2.stderr);
  }
  {
    FileCache cache(temp_dir, 1000, true, false);
    ASSERT_TRUE(cache.Run(1));
    FileCache::Entry entry;
    const HandledSource code("int main() { return 0; }"_l);
    const CommandLine cl("-c"_l);
    const Version version("3.5 (revision 100000)"_l);
    const auto hash = FileCache::Hash(code, {}, cl, version);

    ASSERT_TRUE(cache.Find(hash, &entry));
    EXPECT_EQ(expected_object_code, entry.object);
    EXPECT_EQ(expected_deps, entry.deps);
    EXPECT_EQ(expected_stderr, entry.stderr);
  }
}

TEST(FileCacheTest, UseIndexFromDisk) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{"12345678901234567890123456789012" "-12345678-00000001"_l};

  {
    FileCache cache(temp_dir, FileCache::UNLIMITED, false, true);
    const auto manifest_path =
        AppendExtension(cache.CommonPath(hash), base::kExtManifest);

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
    FileCache cache(temp_dir, FileCache::UNLIMITED, false, true);
    const auto manifest_path =
        AppendExtension(cache.CommonPath(hash), base::kExtManifest);

    ASSERT_TRUE(base::File::Write(manifest_path, "1"_l));
    EXPECT_TRUE(cache.Run(1));
    EXPECT_TRUE(base::File::Exists(manifest_path));
  }
}

}  // namespace cache
}  // namespace dist_clang
