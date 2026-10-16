#include <cache/file_cache.h>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/future.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>

#include <base/file_utils.h>

#include <gtest/gtest.h>
#include <perf/stat_service.h>

#include <sys/stat.h>
#include <utime.h>
#include STL(regex)
#include STL(future)
#include STL(thread)

namespace dist_clang {
namespace cache {

using namespace string;

namespace {

// A hash of an entry: "<32 hex>-<8 hex>-<8 hex>". It's formed in the code:
// the literals of this form look like the API keys of Mailgun to the secret
// scanners.
String EntryHashForTest(ui32 index) {
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08x", index);
  return String(32, '1') + "-" + String(8, '2') + "-" + suffix;
}

// The cache checks only the files, that are left before its start.
void MakeOld(const Path& dir) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    struct utimbuf times = {time(nullptr) - 10, time(nullptr) - 10};
    utime(entry.path().c_str(), &times);
  }
}

// The time of the start of a compilation - like the daemon takes it. The files,
// that are modified later, must be modified after a tick of the clock.
i64 CompilationTime() {
  struct timespec now;
  // The file systems take the time from the coarse clock.
  clock_gettime(CLOCK_REALTIME_COARSE, &now);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return i64(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

ui64 GetStat(perf::proto::Metric::Name name) {
  perf::proto::Report report;
  base::Singleton<perf::StatService>::Get().DumpAll(&report);
  for (const auto& metric : report.metric()) {
    if (metric.name() == name) {
      return metric.value();
    }
  }
  return 0;
}

}  // namespace

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
  FileCache cache(temp_dir, 100000, proto::NONE, false);
  ASSERT_TRUE(cache.Run(3600));

  const auto hash1 = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                     CommandLine("-c"_l), Version("1"_l));
  const auto hash2 = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                     CommandLine("-c -O2"_l), Version("1"_l));
  cache.Store(hash1, FileCache::Entry{"object"_l, "deps"_l, "stderr"_l});
  cache.Store(hash2, FileCache::Entry{"object"_l, "deps"_l});
  cache.new_entries_.reset(new FileCache::EntryList,
                           cache.new_entries_deleter_);
  cache.WaitForCleaner();

  const auto common_path = cache.CommonPath(hash1);
  const auto blob_path =
      cache.blobs_.RawPath(base::Hexify(Immutable("object"_l).Hash()));
  ASSERT_TRUE(base::File::Exists(blob_path));

  // The files of the entry are removed - but not the blob of another one.
  EXPECT_TRUE(cache.RemoveEntry(hash1));
  for (const auto& extension :
       {base::kExtManifest, base::kExtDeps, base::kExtStderr}) {
    EXPECT_FALSE(base::File::Exists(AppendExtension(common_path, extension)));
  }
  EXPECT_FALSE(cache.entries_->Exists(hash1.str));
  EXPECT_TRUE(base::File::Exists(blob_path));

  EXPECT_TRUE(cache.RemoveEntry(hash2));
  EXPECT_FALSE(cache.entries_->Exists(hash2.str));
  EXPECT_FALSE(base::File::Exists(blob_path));
  EXPECT_EQ(0u, cache.cache_size_);
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
  entry1.compilation_time = 1234;

  cache.Store(hash, entry1);

  ASSERT_TRUE(cache.Find(hash, &entry2));
  EXPECT_EQ(expected_object_code, entry2.object);
  EXPECT_EQ(expected_deps, entry2.deps);
  EXPECT_EQ(expected_stderr, entry2.stderr);
  EXPECT_EQ(1234u, entry2.compilation_time);
}

TEST(FileCacheTest, RemoveEntryInBackground) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  FileCache::Entry entry;
  entry.object = "object"_l;
  cache.Store(hash, entry);

  {
    FileCache::Entry found;
    ASSERT_TRUE(cache.Find(hash, &found));
    EXPECT_EQ(hash.str.string_copy(), found.handled_hash);
  }

  cache.Remove(hash);
  bool found = true;
  for (int i = 0; i < 100 && found; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    FileCache::Entry found_entry;
    found = cache.Find(hash, &found_entry);
  }
  EXPECT_FALSE(found);
  EXPECT_FALSE(base::File::Exists(
      AppendExtension(cache.CommonPath(hash), base::kExtObject)));
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
  for (const auto compression : {proto::ZSTD}) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, compression, false);
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

  // The files are really compressed.
  EXPECT_GT(object.size() / 4,
            base::File::Size(AppendExtension(cache.CommonPath(hash),
                                             base::kExtObject)))
      << compression;
  }
}

TEST(FileCacheTest, LinkOutputs) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto dwo_path = temp_dir.path() / "test.dwo";
  const auto restored_path = temp_dir.path() / "restored.o";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  false, true);
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

TEST(FileCacheTest, CompressColdEntries) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto dwo_path = temp_dir.path() / "test.dwo";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  true, true);
  // The cleaner runs only when it's asked to.
  ASSERT_TRUE(cache.Run(3600));

  String object(100000, 'o'), dwo(100000, 'd');
  ASSERT_TRUE(base::File::Write(object_path, Immutable(String(object))));
  ASSERT_TRUE(base::File::Write(dwo_path, Immutable(String(dwo))));

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  FileCache::Entry entry;
  entry.object_path = object_path;
  entry.dwo_path = dwo_path;
  entry.deps = "deps"_l;
  cache.Store(hash, entry);

  // Wait, until the entry gets into the index - and the references to the
  // blobs are counted.
  cache.new_entries_.reset(new FileCache::EntryList,
                           cache.new_entries_deleter_);
  SQLite::Value value;
  for (int i = 0; i < 300 && !cache.entries_->Get(hash.str, &value); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(cache.entries_->Get(hash.str, &value));

  const String object_hash = base::Hexify(Immutable(String(object)).Hash());
  const String dwo_hash = base::Hexify(Immutable(String(dwo)).Hash());

  // Not used for a second.
  cache.SetCompressAfter(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // The outputs are still linked with the build directory.
  EXPECT_EQ(0u, cache.CompressColdEntries());
  EXPECT_TRUE(base::File::Exists(cache.blobs_.RawPath(object_hash)));

  ASSERT_TRUE(base::File::Delete(object_path));
  ASSERT_TRUE(base::File::Delete(dwo_path));
  EXPECT_EQ(2u, cache.CompressColdEntries());
  EXPECT_FALSE(base::File::Exists(cache.blobs_.RawPath(object_hash)));
  EXPECT_GT(object.size() / 10,
            base::File::Size(cache.blobs_.PackedPath(object_hash)));
  EXPECT_GT(dwo.size() / 10,
            base::File::Size(cache.blobs_.PackedPath(dwo_hash)));

  // Already compressed.
  EXPECT_EQ(0u, cache.CompressColdEntries());

  // Restored as contents.
  FileCache::Entry found;
  ASSERT_TRUE(cache.Find(hash, &found));
  EXPECT_TRUE(found.object_path.empty());
  EXPECT_TRUE(found.dwo_path.empty());
  EXPECT_EQ(object, found.object.string_copy());
  EXPECT_EQ(dwo, found.dwo.string_copy());
  EXPECT_EQ("deps"_l, found.deps);
}

TEST(FileCacheTest, DeduplicateOutputs) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  true, true);
  ASSERT_TRUE(cache.Run(3600));

  auto Hash = [](const char* flags) {
    return FileCache::Hash(HandledSource("int main() {}"_l), {},
                           CommandLine(Immutable(String(flags))),
                           Version("1"_l));
  };
  auto Index = [&cache](const auto& hash) {
    cache.new_entries_.reset(new FileCache::EntryList,
                             cache.new_entries_deleter_);
    SQLite::Value value;
    for (int i = 0; i < 300 && !cache.entries_->Get(hash.str, &value); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cache.entries_->Get(hash.str, &value);
  };

  // The same object with different keys - e.g. with other warnings.
  const String object(100000, 'o');
  const String object_hash = base::Hexify(Immutable(String(object)).Hash());
  for (const char* flags : {"-c", "-c -Wall"}) {
    // Every compilation produces a new file.
    base::File::Delete(object_path);
    ASSERT_TRUE(base::File::Write(object_path, Immutable(String(object))));
    FileCache::Entry entry;
    entry.object_path = object_path;
    entry.deps = "deps"_l;
    cache.Store(Hash(flags), entry);
    ASSERT_TRUE(Index(Hash(flags)));
  }

  // One file in the cache - linked with the first output only.
  const Path blob_path = cache.blobs_.RawPath(object_hash);
  struct stat st;
  ASSERT_EQ(0, stat(blob_path.c_str(), &st));
  EXPECT_EQ(1u, st.st_nlink);
  {
    std::lock_guard<Mutex> lock(cache.blob_refs_mutex_);
    EXPECT_EQ(2u, cache.blobs_.Refs(object_hash));
  }

  for (const char* flags : {"-c", "-c -Wall"}) {
    FileCache::Entry found;
    ASSERT_TRUE(cache.Find(Hash(flags), &found));
    EXPECT_EQ(blob_path, found.object_path);
  }

  // The second copy isn't needed - and the copy in the second build
  // directory: the first one only replaces the cache file.
  using M = perf::proto::Metric;
  cache.ReportSizes();
  EXPECT_EQ(object.size(), GetStat(M::DISK_SAVED_BY_DEDUPLICATION));
  EXPECT_EQ(0u, GetStat(M::DISK_SAVED_BY_LINKS));
  ASSERT_TRUE(base::File::Delete(object_path));
  ASSERT_EQ(0, link(blob_path.c_str(), object_path.c_str()));
  cache.ReportSizes();
  EXPECT_EQ(0u, GetStat(M::DISK_SAVED_BY_LINKS));
  EXPECT_EQ(object.size(), GetStat(M::CACHE_SHARED_SIZE));
  const Path second_path = temp_dir.path() / "second.o";
  ASSERT_EQ(0, link(blob_path.c_str(), second_path.c_str()));
  cache.ReportSizes();
  EXPECT_EQ(object.size(), GetStat(M::DISK_SAVED_BY_DEDUPLICATION));
  EXPECT_EQ(object.size(), GetStat(M::DISK_SAVED_BY_LINKS));
  ASSERT_TRUE(base::File::Delete(second_path));

  // The blob is removed with the last entry.
  cache.RemoveEntry(Hash("-c"));
  EXPECT_TRUE(base::File::Exists(blob_path));
  cache.RemoveEntry(Hash("-c -Wall"));
  EXPECT_FALSE(base::File::Exists(blob_path));
}

TEST(FileCacheTest, RemoveUnreferencedBlobs) {
  const base::TemporaryDir temp_dir;
  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  const auto hash2 = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                     CommandLine("-c -O2"_l), Version("1"_l));
  Path blob_path, orphan_path;
  {
    FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
    cache.Store(hash2, FileCache::Entry{"object2"_l, "deps"_l});
    blob_path = cache.blobs_.PackedPath(base::Hexify(Immutable("object"_l).Hash()));
    orphan_path = cache.blobs_.PackedPath(base::Hexify(Immutable("object2"_l).Hash()));
  }
  ASSERT_TRUE(base::File::Exists(blob_path));
  ASSERT_TRUE(base::File::Exists(orphan_path));

  // A crash before the second entry is stored.
  ASSERT_TRUE(base::File::Delete(
      AppendExtension(FileCache(temp_dir.path()).CommonPath(hash2),
                      base::kExtManifest)));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "running", "1"_l));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
  ASSERT_TRUE(cache.Run(3600));
  for (int i = 0; i < 300 && base::File::Exists(orphan_path); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(base::File::Exists(orphan_path));
  EXPECT_TRUE(base::File::Exists(blob_path));

  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  EXPECT_EQ("object"_l, entry.object);
}

namespace {

HandledHash TestHash(const char* flags) {
  return FileCache::Hash(HandledSource("int main() {}"_l), {},
                         CommandLine(Immutable(String(flags))),
                         Version("1"_l));
}

}  // namespace

TEST(FileCacheTest, TagStats) {
  const base::TemporaryDir temp_dir;
  // Makes the cleaner process the stored entries and the tags.
  auto FlushToIndex = [](FileCache& cache, const List<HandledHash>& hashes) {
    cache.new_entries_.reset(new FileCache::EntryList,
                             cache.new_entries_deleter_);
    for (const auto& hash : hashes) {
      SQLite::Value value;
      for (int i = 0; i < 300 && !cache.entries_->Get(hash.str, &value);
           ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    std::promise<void> done;
    cache.cleaner_.Push([&done] { done.set_value(); });
    done.get_future().wait();
  };

  auto cache_ptr = std::make_unique<FileCache>(
      temp_dir, FileCache::UNLIMITED, proto::ZSTD, true);
  auto& cache = *cache_ptr;
  ASSERT_TRUE(cache.Run(3600));

  // The second and the third entries have the same object.
  const auto hash1 = TestHash("-c"), hash2 = TestHash("-c -O1"),
             hash3 = TestHash("-c -O2"), hash4 = TestHash("-c -O3");
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.Store(hash3, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.Store(hash4, FileCache::Entry{"object 4"_l, "deps"_l});
  cache.AddTags(hash1, {"a"});
  cache.AddTags(hash2, {"a"});
  cache.AddTags(hash2, {"b"});
  cache.AddTags(hash3, {"b"});
  FlushToIndex(cache, {hash1, hash2, hash3, hash4});

  auto EntrySize = [&](const HandledHash& hash) {
    SQLite::Value value;
    EXPECT_TRUE(cache.entries_->Get(hash.str, &value));
    return std::get<SQLite::SIZE>(value);
  };
  auto BlobSize = [&](const char* content) {
    return cache.blobs_.Size(base::Hexify(Immutable(String(content)).Hash()));
  };

  List<FileCache::TagStats> stats;
  List<FileCache::TagOverlap> overlaps;
  cache.ComputeTagStats(&stats, &overlaps);
  ASSERT_EQ(3u, stats.size());

  auto it = stats.begin();
  EXPECT_EQ("", it->tag);
  EXPECT_EQ(1u, it->entries);
  EXPECT_EQ(EntrySize(hash4) + BlobSize("object 4"), it->size);
  EXPECT_EQ(it->size, it->exclusive_size);

  ++it;
  EXPECT_EQ("a", it->tag);
  EXPECT_EQ(2u, it->entries);
  EXPECT_EQ(1u, it->exclusive_entries);
  EXPECT_EQ(EntrySize(hash1) + EntrySize(hash2) + BlobSize("object 1") +
                BlobSize("object 2"),
            it->size);
  EXPECT_EQ(EntrySize(hash1) + BlobSize("object 1"), it->exclusive_size);
  EXPECT_EQ(it->exclusive_size, it->freeable_size);
  EXPECT_EQ(0u, it->linked_size);

  ++it;
  EXPECT_EQ("b", it->tag);
  EXPECT_EQ(2u, it->entries);
  EXPECT_EQ(1u, it->exclusive_entries);
  // The object is counted once.
  EXPECT_EQ(EntrySize(hash2) + EntrySize(hash3) + BlobSize("object 2"),
            it->size);
  EXPECT_EQ(EntrySize(hash3), it->exclusive_size);
  // The object stays with the other tag.
  EXPECT_EQ(EntrySize(hash3), it->freeable_size);

  ASSERT_EQ(1u, overlaps.size());
  EXPECT_EQ("a", overlaps.front().first);
  EXPECT_EQ("b", overlaps.front().second);
  EXPECT_EQ(EntrySize(hash2) + BlobSize("object 2"), overlaps.front().size);

  // The tags survive the restart.
  cache_ptr.reset();
  FileCache new_cache(temp_dir, FileCache::UNLIMITED, proto::ZSTD, true);
  ASSERT_TRUE(new_cache.Run(3600));
  FlushToIndex(new_cache, {});
  List<FileCache::TagStats> new_stats;
  List<FileCache::TagOverlap> new_overlaps;
  new_cache.ComputeTagStats(&new_stats, &new_overlaps);
  EXPECT_EQ(3u, new_stats.size());
  EXPECT_EQ(1u, new_overlaps.size());
}

TEST(FileCacheTest, ForgetTag) {
  const base::TemporaryDir temp_dir;
  // Makes the cleaner process the stored entries and the tags.
  auto FlushToIndex = [](FileCache& cache, const List<HandledHash>& hashes) {
    cache.new_entries_.reset(new FileCache::EntryList,
                             cache.new_entries_deleter_);
    for (const auto& hash : hashes) {
      SQLite::Value value;
      for (int i = 0; i < 300 && !cache.entries_->Get(hash.str, &value);
           ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    std::promise<void> done;
    cache.cleaner_.Push([&done] { done.set_value(); });
    done.get_future().wait();
  };

  const auto object_path = temp_dir.path() / "test.o";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  true, true);
  ASSERT_TRUE(cache.Run(3600));

  const auto hash1 = TestHash("-c"), hash2 = TestHash("-c -O1"),
             hash3 = TestHash("-c -O2"), hash4 = TestHash("-c -O3");
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.Store(hash3, FileCache::Entry{"object 2"_l, "deps"_l});
  // The output of the current build.
  ASSERT_TRUE(base::File::Write(object_path, "object 4"_l));
  FileCache::Entry linked;
  linked.object_path = object_path;
  linked.deps = "deps"_l;
  cache.Store(hash4, linked);
  cache.AddTags(hash1, {"a"});
  cache.AddTags(hash2, {"a", "b"});
  cache.AddTags(hash3, {"b"});
  cache.AddTags(hash4, {"a"});
  FlushToIndex(cache, {hash1, hash2, hash3, hash4});

  auto EntrySize = [&](const HandledHash& hash) {
    SQLite::Value value;
    EXPECT_TRUE(cache.entries_->Get(hash.str, &value));
    return std::get<SQLite::SIZE>(value);
  };
  auto BlobPath = [&](const char* content) {
    return cache.blobs_.RawPath(base::Hexify(Immutable(String(content)).Hash()));
  };
  auto Exists = [&](const HandledHash& hash) {
    SQLite::Value value;
    return cache.entries_->Get(hash.str, &value);
  };

  const ui64 size1 = EntrySize(hash1) + base::File::Size(BlobPath("object 1"));

  // The output of the build directory is a part of the size of the tag, but
  // isn't freed with it.
  List<FileCache::TagStats> stats;
  List<FileCache::TagOverlap> overlaps;
  cache.ComputeTagStats(&stats, &overlaps);
  ASSERT_EQ(2u, stats.size());
  EXPECT_EQ("a", stats.front().tag);
  EXPECT_EQ(base::File::Size(BlobPath("object 4")), stats.front().linked_size);
  EXPECT_EQ(size1 + EntrySize(hash4) + base::File::Size(BlobPath("object 4")),
            stats.front().exclusive_size);
  EXPECT_EQ(size1, stats.front().freeable_size);

  // Nothing is removed on the dry run.
  auto result = cache.DoForgetTag("a", true);
  EXPECT_EQ(1u, result.removed_entries);
  EXPECT_EQ(size1, result.freed_size);
  EXPECT_EQ(1u, result.shared_entries);
  EXPECT_EQ(1u, result.linked_entries);
  EXPECT_TRUE(Exists(hash1));

  result = cache.DoForgetTag("a", false);
  EXPECT_EQ(1u, result.removed_entries);
  EXPECT_FALSE(Exists(hash1));
  EXPECT_FALSE(base::File::Exists(BlobPath("object 1")));
  EXPECT_TRUE(Exists(hash2));
  EXPECT_TRUE(Exists(hash4));
  EXPECT_EQ(Vector<String>{"b"}, cache.entry_tags_[hash2.str.string_copy()]);

  // The shared object is freed with the last entry.
  const ui64 size23 = EntrySize(hash2) + EntrySize(hash3) +
                      base::File::Size(BlobPath("object 2"));
  result = cache.DoForgetTag("b", false);
  EXPECT_EQ(2u, result.removed_entries);
  EXPECT_EQ(size23, result.freed_size);
  EXPECT_FALSE(base::File::Exists(BlobPath("object 2")));

  // Not linked anymore.
  ASSERT_TRUE(base::File::Delete(object_path));
  result = cache.DoForgetTag("a", false);
  EXPECT_EQ(1u, result.removed_entries);
  EXPECT_FALSE(Exists(hash4));
}

TEST(FileCacheTest, Format) {
  const base::TemporaryDir temp_dir;
  const Path cache_path = temp_dir.path() / "cache";
  const Path format_path = cache_path / "format";
  const auto hash = TestHash("-c");

  auto ReadFormat = [&] {
    proto::Format format;
    EXPECT_TRUE(base::LoadFromFile(format_path, &format));
    return format;
  };
  auto WriteFormat = [&](ui32 generation, ui32 revision) {
    proto::Format format;
    format.set_generation(generation);
    format.set_revision(revision);
    ASSERT_TRUE(base::SaveToFile(format_path, format));
  };
  auto Found = [&] {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::ZSTD, true);
    if (!cache.Run(3600)) {
      return false;
    }
    FileCache::Entry entry;
    return cache.Find(hash, &entry);
  };

  // A new cache gets the current format.
  {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::ZSTD, true);
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  }
  EXPECT_EQ(FileCache::kGeneration, ReadFormat().generation());
  EXPECT_EQ(FileCache::kRevision, ReadFormat().revision());
  EXPECT_FALSE(ReadFormat().written_by().empty());

  // The cache before the generations is the first one - it's removed, but
  // not the other files in its directory.
  const Path foreign_path = cache_path / "foreign.txt";
  ASSERT_TRUE(base::File::Write(foreign_path, "foreign"_l));
  ASSERT_TRUE(base::File::Delete(format_path));
  EXPECT_FALSE(Found());
  EXPECT_EQ(FileCache::kGeneration, ReadFormat().generation());
  EXPECT_TRUE(base::File::Exists(foreign_path));
  {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::ZSTD, true);
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  }
  EXPECT_TRUE(Found());

  // The newer revision of the same generation is used - and stays.
  WriteFormat(FileCache::kGeneration, FileCache::kRevision + 1);
  EXPECT_TRUE(Found());
  EXPECT_EQ(FileCache::kRevision + 1, ReadFormat().revision());

  // The newer generation isn't used - and isn't touched.
  WriteFormat(FileCache::kGeneration + 1, 0);
  EXPECT_FALSE(Found());
  EXPECT_EQ(FileCache::kGeneration + 1, ReadFormat().generation());
  WriteFormat(FileCache::kGeneration, FileCache::kRevision);
  EXPECT_TRUE(Found());

  // The older generation is removed.
  WriteFormat(FileCache::kGeneration - 1, 0);
  EXPECT_FALSE(Found());
  EXPECT_EQ(FileCache::kGeneration, ReadFormat().generation());
  // In background - the cache aside of the new one.
  bool removed = false;
  for (int i = 0; i < 300 && !removed; ++i) {
    {
      // The next start removes the leftovers too.
      FileCache cache(cache_path, FileCache::UNLIMITED, proto::ZSTD, true);
      ASSERT_TRUE(cache.Run(3600));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    removed = true;
    for (const auto& entry :
         std::filesystem::directory_iterator(temp_dir.path())) {
      removed &= entry.path() == cache_path;
    }
    removed &= base::File::Exists(foreign_path);
  }
  EXPECT_TRUE(removed);
}

TEST(FileCacheTest, ForgetUntagged) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::ZSTD, true);
  ASSERT_TRUE(cache.Run(3600));

  const auto hash1 = TestHash("-c"), hash2 = TestHash("-c -O1");
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.AddTags(hash2, {"a"});

  // A request of the client, that is gone, isn't done.
  ASSERT_TRUE(cache.ForgetTag(String(), false,
                              [](const FileCache::ForgetResult&) {
                                ADD_FAILURE() << "Not needed anymore";
                              },
                              [] { return false; }));

  std::promise<FileCache::ForgetResult> dry_run;
  ASSERT_TRUE(cache.ForgetTag(String(), true,
                              [&](const FileCache::ForgetResult& result) {
                                dry_run.set_value(result);
                              }));
  EXPECT_EQ(1u, dry_run.get_future().get().removed_entries);

  std::promise<FileCache::ForgetResult> forgotten;
  ASSERT_TRUE(cache.ForgetTag(String(), false,
                              [&](const FileCache::ForgetResult& result) {
                                forgotten.set_value(result);
                              }));
  EXPECT_EQ(1u, forgotten.get_future().get().removed_entries);

  FileCache::Entry entry1, entry2;
  EXPECT_FALSE(cache.Find(hash1, &entry1));
  EXPECT_TRUE(cache.Find(hash2, &entry2));
}

TEST(FileCacheTest, TagStatsDontWaitForMaintenance) {
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  true, true);
  ASSERT_TRUE(cache.Run(3600));

  // An unused output, that isn't linked anymore.
  const String object(100000, 'o');
  ASSERT_TRUE(base::File::Write(object_path, Immutable(String(object))));
  const auto hash = TestHash("-c");
  FileCache::Entry entry;
  entry.object_path = object_path;
  entry.deps = "deps"_l;
  cache.Store(hash, entry);
  ASSERT_TRUE(base::File::Delete(object_path));
  cache.SetCompressAfter(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // The statistics only apply the accesses - the requests, that wait at the
  // same time, get the same ones.
  std::promise<ui64> first, second;
  ASSERT_TRUE(cache.GetTagStats(
      [&](const List<FileCache::TagStats>& stats,
          const List<FileCache::TagOverlap>&) {
        first.set_value(stats.size() ? stats.front().entries : 0);
      }));
  ASSERT_TRUE(cache.GetTagStats(
      [&](const List<FileCache::TagStats>& stats,
          const List<FileCache::TagOverlap>&) {
        second.set_value(stats.size() ? stats.front().entries : 0);
      }));
  EXPECT_EQ(1u, first.get_future().get());
  EXPECT_EQ(1u, second.get_future().get());

  const auto raw_path =
      cache.blobs_.RawPath(base::Hexify(Immutable(String(object)).Hash()));
  EXPECT_TRUE(base::File::Exists(raw_path));

  // The maintenance compresses it.
  std::promise<void> done;
  cache.cleaner_.Push([&] {
    cache.Maintain();
    done.set_value();
  });
  done.get_future().wait();
  EXPECT_FALSE(base::File::Exists(raw_path));

  // Nothing is compressed after the deadline.
  std::lock_guard<Mutex> lock(cache.blob_refs_mutex_);
  EXPECT_EQ(0u, cache.blobs_.CompressUnused(time(nullptr), FileCache::UNLIMITED,
                                            Clock::now()));
}

TEST(FileCacheTest, LinkOutputsWithContents) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
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

TEST(FileCacheTest, DetectBrokenFiles) {
  for (const auto compression : {proto::NONE, proto::ZSTD}) {
    for (const auto* extension : {"o", "d", "stderr", "dwo"}) {
      const base::TemporaryDir temp_dir;
      FileCache cache(temp_dir.path(), FileCache::UNLIMITED, compression,
                      false);
      ASSERT_TRUE(cache.Run(1));

      const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                        CommandLine("-c"_l), Version("1"_l));
      cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, "stderr"_l,
                                         "dwo"_l});

      // Flip a byte, keeping the size - like a broken write. The outputs are
      // in the blob store.
      Path path = cache.CommonPath(hash).string() + "." + extension;
      if (extension == String("o") || extension == String("dwo")) {
        const String blob_hash = base::Hexify(
            Immutable(extension == String("o") ? "object"_l : "dwo"_l).Hash());
        path = cache.blobs_.RawPath(blob_hash);
        if (!base::File::Exists(path)) {
          path = cache.blobs_.PackedPath(blob_hash);
        }
      }
      Immutable content;
      ASSERT_TRUE(base::File::Read(path, &content));
      String broken = content.string_copy();
      broken[0] ^= 1;
      ASSERT_EQ(0, chmod(path.c_str(), S_IRUSR | S_IWUSR));
      ASSERT_TRUE(base::File::Write(path, Immutable(std::move(broken))));

      FileCache::Entry entry;
      EXPECT_FALSE(cache.Find(hash, &entry))
          << "compression: " << compression << ", file: " << extension;
    }
  }
}

TEST(FileCacheTest, EntryWithoutHashes) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, "stderr"_l});

  // E.g. a manifest, written partially before a crash of the system.
  const auto manifest_path = cache.CommonPath(hash).string() + ".manifest";
  proto::Manifest manifest;
  ASSERT_TRUE(base::LoadFromFile(manifest_path, &manifest));
  manifest.mutable_v1()->clear_object_hash();
  ASSERT_TRUE(base::SaveToFile(manifest_path, manifest));

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(hash, &entry));
}

TEST(FileCacheTest, RemoveStaleTemporaryFiles) {
  const base::TemporaryDir temp_dir;
  const auto tmp1 = temp_dir.path() / "index.sqlite.tmp";
  const auto tmp2 = temp_dir.path() / "a" / "b" / "hash.o.dc-123-4.tmp";
  const auto kept = temp_dir.path() / "a" / "b" / "hash.o";
  ASSERT_TRUE(base::CreateDirectory(temp_dir.path() / "a" / "b"));
  for (const auto& path : {tmp1, tmp2, kept}) {
    ASSERT_TRUE(base::File::Write(path, "garbage"_l));
  }

  // A crash in the middle of storing: the files without the manifest.
  const String orphan_hash = EntryHashForTest(1);
  const auto orphan1 = temp_dir.path() / "a" / "b" / (orphan_hash + ".d");
  const auto orphan2 = temp_dir.path() / "a" / "b" / (orphan_hash + ".stderr");
  for (const auto& path : {orphan1, orphan2}) {
    ASSERT_TRUE(base::File::Write(path, "garbage"_l));
  }
  // They are left before the start - the new ones may be written right now.
  const auto fresh_tmp = temp_dir.path() / "a" / "b" / "hash.o.dc-123-5.tmp";
  for (const auto& path : {tmp1, tmp2, kept, orphan1, orphan2}) {
    struct utimbuf times = {time(nullptr) - 10, time(nullptr) - 10};
    ASSERT_EQ(0, utime(path.c_str(), &times));
  }

  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  ASSERT_TRUE(base::File::Write(fresh_tmp, "garbage"_l));
  cache.WaitForCleaner();
  EXPECT_TRUE(base::File::Exists(fresh_tmp));

  EXPECT_FALSE(base::File::Exists(tmp1));
  EXPECT_FALSE(base::File::Exists(tmp2));
  EXPECT_FALSE(base::File::Exists(orphan1));
  EXPECT_FALSE(base::File::Exists(orphan2));
  EXPECT_TRUE(base::File::Exists(kept));

  // The complete entries are kept.
  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, "stderr"_l});
  FileCache cache2(temp_dir);
  ASSERT_TRUE(cache2.Run(1));
  cache2.WaitForCleaner();
  FileCache::Entry entry;
  EXPECT_TRUE(cache2.Find(hash, &entry));
}

TEST(FileCacheTest, RecreateBrokenDatabases) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "direct.sqlite",
                                "not a database at all"_l));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "index.sqlite",
                                "not a database at all"_l));

  FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::NONE, true);
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("1"_l);
  const auto hash = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, Immutable()});
  const UnhandledSource code("int main() {}"_l);
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.h", "#define A"_l));
  cache.Store(code, {}, cl, version, {"a.h"}, {}, temp_dir.path(), hash);

  FileCache::Entry entry;
  EXPECT_TRUE(cache.Find(code, {}, cl, version, temp_dir.path(), &entry));
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
    FileCache probe(probe_dir.path() / "cache", FileCache::UNLIMITED,
                    proto::NONE, false);
    ASSERT_TRUE(probe.Run(1));
    const auto probe_db_size = probe.MetadataSize();
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
  FileCache cache(cache_path, entry_size[0] + entry_size[1], proto::NONE, false);

  ASSERT_TRUE(cache.Run(1));
  auto db_size = cache.MetadataSize();

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
    // The records of the evicted entries are added.
    EXPECT_EQ(entry_size[2],
              base::CalculateDirectorySize(cache_path) - cache.MetadataSize());
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
    FileCache probe(probe_dir.path() / "cache", FileCache::UNLIMITED,
                    proto::NONE, false);
    ASSERT_TRUE(probe.Run(1));
    const auto probe_db_size = probe.MetadataSize();
    for (int i = 0; i < 3; ++i) {
      probe.Store(Hash(i), FileCache::Entry{obj_content[i], String(), String()});
    }
    total_size = base::CalculateDirectorySize(probe_dir.path() / "cache") -
                 probe_db_size;
  }

  // Only one entry doesn't fit.
  FileCache cache(cache_path, total_size - 1, proto::NONE, false);
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
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
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

TEST(FileCacheTest, DirectEntry_SourceRoot) {
  const base::TemporaryDir temp_dir;
  const auto root1 = temp_dir.path() / "root1";
  const auto root2 = temp_dir.path() / "root2";
  ASSERT_TRUE(base::CreateDirectory(root1));
  ASSERT_TRUE(base::CreateDirectory(root2));
  ASSERT_TRUE(base::File::Write(root1 / "a.h", "#define A"_l));
  ASSERT_TRUE(base::File::Write(root2 / "a.h", "#define A"_l));

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c -I<ROOT>"_l);
  const Version version("1"_l);
  const UnhandledSource code("#include \"a.h\""_l);
  const auto hash = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, Immutable()});

  // The header is given relative to the build directory, which is outside of
  // the root.
  const auto build_dir = temp_dir.path() / "build";
  ASSERT_TRUE(base::CreateDirectory(build_dir));
  cache.Store(code, {}, cl, version, {"../root1/a.h"}, {}, build_dir, hash,
              root1.string());

  FileCache::Entry entry1;
  EXPECT_TRUE(cache.Find(code, {}, cl, version, build_dir, &entry1,
                         root2.string()));

  // The header from the other root is used.
  ASSERT_TRUE(base::File::Write(root2 / "a.h", "#define B"_l));
  FileCache::Entry entry2;
  EXPECT_FALSE(cache.Find(code, {}, cl, version, build_dir, &entry2,
                          root2.string()));
  FileCache::Entry entry3;
  EXPECT_TRUE(cache.Find(code, {}, cl, version, build_dir, &entry3,
                         root1.string()));

  // Without the root it's not found.
  FileCache::Entry entry4;
  EXPECT_FALSE(cache.Find(code, {}, cl, version, build_dir, &entry4));
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

TEST(FileCacheTest, DirectEntry_TimeMacros) {
  const base::TemporaryDir temp_dir;
  const auto source_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "test.h";
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);

  FileCache::Entry stored_entry;
  stored_entry.object = "some object code"_l;
  stored_entry.deps = "some deps"_l;
  cache.Store(hash, stored_entry);

  auto FindDirect = [&] {
    FileCache::Entry entry;
    return cache.Find(orig_code, {}, cl, version, temp_dir, &entry);
  };

  ASSERT_TRUE(base::File::Write(source_path, orig_code.str));

  // The result depends on the time of the compilation - only the simple cache
  // is fine: the macros are expanded in the preprocessed source.
  for (const auto macro : {"__DATE__"_l, "__TIME__"_l, "__TIMESTAMP__"_l}) {
    ASSERT_TRUE(base::File::Write(header_path,
                                  Immutable("const char* t = "_l) + macro));
    cache.Store(orig_code, {}, cl, version, {source_path, header_path}, {},
                temp_dir, hash);
    EXPECT_FALSE(FindDirect())
        << macro;
    FileCache::Entry entry;
    EXPECT_TRUE(cache.Find(hash, &entry));
  }

  // The same with the macro in the source itself.
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(source_path, "const char* t = __DATE__;"_l));
  cache.Store(orig_code, {}, cl, version, {source_path, header_path}, {},
              temp_dir, hash);
  EXPECT_FALSE(FindDirect());

  // Without the macros the direct entry is stored.
  ASSERT_TRUE(base::File::Write(source_path, orig_code.str));
  cache.Store(orig_code, {}, cl, version, {source_path, header_path}, {},
              temp_dir, hash);
  EXPECT_TRUE(FindDirect());
}

TEST(FileCacheTest, ShadowingPaths) {
  const List<Path> dirs = {"/src/dir", "/src", "/build/gen", "/src/inc"};

  // The header may be found in "/src" as "inc/a.h" or in "/src/inc" as "a.h".
  EXPECT_EQ((List<Path>{"/src/dir/inc/a.h", "/src/dir/a.h", "/src/a.h",
                        "/build/gen/a.h"}),
            FileCache::ShadowingPaths("/src/inc/a.h", dirs));
  EXPECT_EQ((List<Path>{"/src/dir/b/c.h", "/src/b/c.h"}),
            FileCache::ShadowingPaths("/build/gen/b/c.h", dirs));
  // Nothing is searched before the first directory - but the header may be
  // found also in "/src" as "dir/d.h".
  EXPECT_EQ((List<Path>{"/src/dir/dir/d.h"}),
            FileCache::ShadowingPaths("/src/dir/d.h", dirs));
  EXPECT_EQ(List<Path>(), FileCache::ShadowingPaths("/src/dir/d.h",
                                                    {"/src/dir", "/build"}));
  // The header isn't from the search directories.
  EXPECT_EQ(List<Path>(), FileCache::ShadowingPaths("/usr/include/e.h", dirs));
  EXPECT_EQ(List<Path>(), FileCache::ShadowingPaths("/src/dirx.h", {"/src/dir"}));
}

TEST(FileCacheTest, DirectEntry_ShadowedHeader) {
  const base::TemporaryDir temp_dir;
  const Path dir1 = temp_dir.path() / "dir1";
  const Path dir2 = temp_dir.path() / "dir2";
  ASSERT_TRUE(base::CreateDirectory(dir1 / "sub"));
  ASSERT_TRUE(base::CreateDirectory(dir2 / "sub"));
  const List<Path> search_dirs = {dir1, dir2};
  const auto header1_path = dir2 / "sub" / "a.h";
  const auto header2_path = dir2 / "b.h";
  const auto shadow1_path = dir1 / "sub" / "a.h";
  const auto shadow2_path = dir1 / "b.h";

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);

  FileCache::Entry stored_entry;
  stored_entry.object = "some object code"_l;
  stored_entry.deps = "some deps"_l;
  cache.Store(hash, stored_entry);

  auto FindDirect = [&] {
    FileCache::Entry entry;
    return cache.Find(orig_code, {}, cl, version, temp_dir, &entry, String(),
                      search_dirs);
  };

  // The second header has a file with the same name before it - which
  // isn't used, e.g. because of "#include_next".
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));
  ASSERT_TRUE(base::File::Write(shadow2_path, "#define C"_l));
  cache.Store(orig_code, {}, cl, version, {header1_path, header2_path}, {},
              temp_dir, hash, String(), search_dirs,
              CompilationTime());
  EXPECT_TRUE(FindDirect());

  // A new file would be found instead of the first header.
  ASSERT_TRUE(base::File::Write(shadow1_path, "#define D"_l));
  EXPECT_FALSE(FindDirect());
  // Also without the search directories nothing is checked.
  {
    FileCache::Entry entry;
    EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
  }

  // The directories don't shadow headers.
  ASSERT_TRUE(base::File::Delete(shadow1_path));
  ASSERT_TRUE(base::CreateDirectory(shadow1_path));
  EXPECT_TRUE(FindDirect());
  ASSERT_EQ(0, rmdir(shadow1_path.c_str()));
  EXPECT_TRUE(FindDirect());

  // The ignored file is still fine after it's re-created.
  ASSERT_TRUE(base::File::Delete(shadow2_path));
  ASSERT_TRUE(base::File::Write(shadow2_path, "#define E"_l));
  EXPECT_TRUE(FindDirect());
}

TEST(FileCacheTest, DirectEntry_DontStoreIfShadowAppearsDuringCompilation) {
  const base::TemporaryDir temp_dir;
  const Path dir1 = temp_dir.path() / "dir1";
  const Path dir2 = temp_dir.path() / "dir2";
  ASSERT_TRUE(base::CreateDirectory(dir1));
  ASSERT_TRUE(base::CreateDirectory(dir2));
  const List<Path> search_dirs = {dir1, dir2};
  const auto header_path = dir2 / "a.h";

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);

  FileCache::Entry stored_entry;
  stored_entry.object = "some object code"_l;
  stored_entry.deps = "some deps"_l;
  cache.Store(hash, stored_entry);

  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  const i64 start_time = CompilationTime();

  // The compiler may have missed the file, that appears during the
  // compilation - nothing is known about the result.
  ASSERT_TRUE(base::File::Write(dir1 / "a.h", "#define B"_l));
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash,
              String(), search_dirs, start_time);

  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry,
                          String(), search_dirs));
}

TEST(FileCacheTest, DirectEntry_DontStoreIfHeaderModifiedDuringCompilation) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);
  FileCache::Entry stored_entry;
  stored_entry.object = "some object code"_l;
  stored_entry.deps = "some deps"_l;
  cache.Store(hash, stored_entry);

  // The direct cache is updated after the compilation - the header may be
  // edited since its start, and the compiler may have read its old version.
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  const i64 start_time = CompilationTime();
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash,
              String(), {}, start_time);
  {
    FileCache::Entry entry;
    EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
  }

  // Not modified since the start.
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash,
              String(), {}, CompilationTime());
  FileCache::Entry entry;
  EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
}

TEST(FileHashCacheTest, TimeMacrosAndModificationTime) {
  const base::TemporaryDir temp_dir;
  const auto path = temp_dir.path() / "test.h";
  FileHashCache hashes;

  for (const auto& [content, expected] :
       List<Pair<Literal, bool>>{{"#define A"_l, false},
                                 {"const char* t = __TIME__;"_l, true}}) {
    ASSERT_TRUE(base::File::Write(path, content));
    String hash;
    bool time_macros = !expected;
    i64 modified = 0;
    ASSERT_TRUE(hashes.Hash(path, &hash, nullptr, &time_macros, &modified));
    EXPECT_EQ(base::Hexify(Immutable(content).Hash()), hash);
    EXPECT_EQ(expected, time_macros);
    struct stat st;
    ASSERT_EQ(0, stat(path.c_str(), &st));
    EXPECT_EQ(std::max(i64(st.st_mtim.tv_sec) * 1'000'000'000 +
                           st.st_mtim.tv_nsec,
                       i64(st.st_ctim.tv_sec) * 1'000'000'000 +
                           st.st_ctim.tv_nsec),
              modified);
  }
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

TEST(FileCacheTest, CheckFilesOnlyAfterCrash) {
  const base::TemporaryDir temp_dir;
  string::Hash hash{Immutable(EntryHashForTest(1))};
  const auto old_entry = [&] {
    FileCache cache(temp_dir);
    return AppendExtension(cache.CommonPath(hash), base::kExtManifest);
  }();

  {
    // The manifest of an old version isn't migrated - it's removed.
    FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
    ASSERT_TRUE(base::CreateDirectory(cache.SecondPath(hash)));
    proto::Manifest manifest;
    manifest.set_version(1);
    manifest.mutable_v1()->set_size(0);
    ASSERT_TRUE(base::SaveToFile(old_entry, manifest));
    MakeOld(temp_dir);
    // The new index - the files are checked.
    EXPECT_TRUE(cache.Run(1));
    cache.WaitForCleaner();
    EXPECT_FALSE(base::File::Exists(old_entry));
    EXPECT_TRUE(base::File::Exists(temp_dir.path() / "running"));
  }
  EXPECT_FALSE(base::File::Exists(temp_dir.path() / "running"));

  // The stop on a signal is the normal one too.
  {
    FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
    EXPECT_TRUE(cache.Run(1));
    cache.Close();
    EXPECT_FALSE(base::File::Exists(temp_dir.path() / "running"));
  }

  // After the normal stop the files aren't checked.
  ASSERT_TRUE(base::File::Write(old_entry, "1"_l));
  MakeOld(temp_dir);
  {
    FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
    EXPECT_TRUE(cache.Run(1));
    cache.WaitForCleaner();
    EXPECT_TRUE(base::File::Exists(old_entry));
  }

  // After a crash they are.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "running", "1"_l));
  MakeOld(temp_dir);
  {
    FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
    EXPECT_TRUE(cache.Run(1));
    cache.WaitForCleaner();
    EXPECT_FALSE(base::File::Exists(old_entry));
  }
}

TEST(FileCacheTest, ReportSizes) {
  using M = perf::proto::Metric;
  const base::TemporaryDir temp_dir;
  const auto hash1 = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                     CommandLine("-c"_l), Version("1"_l));
  const auto hash2 = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                     CommandLine("-c -O2"_l), Version("1"_l));

  {
    FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
    cache.flush_period_ = 1;
    ASSERT_TRUE(cache.Run(3600));
    cache.WaitForCleaner();
    EXPECT_EQ(0u, GetStat(M::CACHE_ENTRIES));
    EXPECT_EQ(0u, GetStat(M::CACHE_OWN_SIZE));

    // The accesses are applied without waiting for the maintenance.
    const ui64 added = GetStat(M::CACHE_SIZE_ADDED);
    cache.Store(hash1, FileCache::Entry{"object"_l, "deps"_l});
    for (int i = 0; i < 30 && GetStat(M::CACHE_SIZE_ADDED) == added; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_LT(added, GetStat(M::CACHE_SIZE_ADDED));
    cache.Store(hash2, FileCache::Entry{"object2"_l, "deps"_l});
  }

  // The sizes are known after the start - without waiting for the maintenance.
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
  ASSERT_TRUE(cache.Run(3600));
  cache.WaitForCleaner();
  EXPECT_EQ(2u, GetStat(M::CACHE_ENTRIES));
  EXPECT_LT("object"_l.size() + "object2"_l.size(),
            GetStat(M::CACHE_OWN_SIZE));
  EXPECT_EQ(0u, GetStat(M::CACHE_SHARED_SIZE));

  EXPECT_TRUE(cache.RemoveEntry(hash1));
  cache.ReportSizes();
  EXPECT_EQ(1u, GetStat(M::CACHE_ENTRIES));
}

TEST(FileCacheTest, Ghosts) {
  using M = perf::proto::Metric;
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true);
  ASSERT_TRUE(cache.Run(3600));

  // Makes the cleaner process the stored entries and the tags.
  auto Flush = [&cache] {
    cache.new_entries_.reset(new FileCache::EntryList,
                             cache.new_entries_deleter_);
    cache.WaitForCleaner();
  };
  auto Store = [&](const char* flags, const char* object, ui64 time) {
    FileCache::Entry entry{Immutable(String(object)), "deps"_l};
    entry.compilation_time = time;
    cache.Store(TestHash(flags), entry);
  };
  auto RunInCleaner = [&cache](Fn<void()> task) {
    std::promise<void> done;
    cache.cleaner_.Push([&] {
      task();
      done.set_value();
    });
    done.get_future().wait();
  };

  Store("-c", "object 1", 500);
  Store("-c -O1", "object 2", 700);
  Flush();

  // Both entries are evicted.
  RunInCleaner([&] {
    cache.max_size_ = 1;
    cache.Evict();
    cache.max_size_ = FileCache::UNLIMITED;
  });
  FileCache::Entry entry;
  ASSERT_FALSE(cache.Find(TestHash("-c"), &entry));
  ASSERT_FALSE(cache.Find(TestHash("-c -O1"), &entry));
  EXPECT_EQ(2u, cache.ghosts_->Count());

  // The same object is compiled again - with other flags too: the evicted
  // entry would have been found.
  Store("-c -O2", "object 1", 900);
  Flush();
  auto losses = cache.ghosts_->GetLosses();
  ASSERT_EQ(1u, losses.size());
  EXPECT_EQ(GhostIndex::EVICTED, losses.front().reason);
  EXPECT_EQ(900u, losses.front().compilation_time);
  EXPECT_LT(0u, losses.front().extra_size);
  EXPECT_EQ(1u, cache.ghosts_->Count());

  // The entry exists again - another compilation isn't a loss.
  Store("-c -O3", "object 1", 900);
  Flush();
  EXPECT_EQ(1u, cache.ghosts_->GetLosses().size());

  RunInCleaner([&] { cache.ReportSizes(); });
  EXPECT_EQ(1u, GetStat(M::EVICTED_NEEDED_AGAIN));
  EXPECT_EQ(900u, GetStat(M::TIME_LOST_TO_EVICTION));
  EXPECT_EQ(losses.front().extra_size,
            GetStat(M::EXTRA_SIZE_FOR_ALL_LOSSES));
  EXPECT_EQ(1u, GetStat(M::GHOSTS));

  // The forgotten entry keeps its tag - the tag has no entries anymore.
  Store("-c -g", "object 3", 300);
  cache.AddTags(TestHash("-c -g"), {"branch"});
  Flush();
  RunInCleaner([&] { cache.DoForgetTag("branch", false); });
  Store("-c -g -O1", "object 3", 400);
  Flush();
  losses = cache.ghosts_->GetLosses();
  ASSERT_EQ(2u, losses.size());
  EXPECT_EQ(GhostIndex::FORGOTTEN, losses.back().reason);
  EXPECT_EQ(Vector<String>{"branch"}, losses.back().tags);
  EXPECT_EQ(0u, losses.back().extra_size);

  List<FileCache::TagStats> stats;
  List<FileCache::TagOverlap> overlaps;
  RunInCleaner([&] { cache.ComputeTagStats(&stats, &overlaps); });
  auto branch = std::find_if(stats.begin(), stats.end(),
                             [](const auto& stat) { return stat.tag == "branch"; });
  ASSERT_NE(stats.end(), branch);
  EXPECT_EQ(0u, branch->entries);
  EXPECT_EQ(1u, branch->lost_entries);
  EXPECT_EQ(400u, branch->lost_time);

  // The records are forgotten - with the losses.
  std::promise<ui64> forgotten;
  ASSERT_TRUE(cache.ForgetGhosts(
      [&forgotten](ui64 count) { forgotten.set_value(count); }));
  EXPECT_EQ(1u, forgotten.get_future().get());
  EXPECT_EQ(0u, cache.ghosts_->Count());
  EXPECT_TRUE(cache.ghosts_->GetLosses().empty());
  EXPECT_EQ(0u, GetStat(M::EVICTED_NEEDED_AGAIN));

  // The old records expire.
  Store("-c -g -O2", "object 4", 300);
  Flush();
  RunInCleaner([&] {
    cache.max_size_ = 1;
    cache.Evict();
    cache.max_size_ = FileCache::UNLIMITED;
  });
  EXPECT_LT(0u, cache.ghosts_->Count());
  cache.ghosts_->RemoveOlderThan(time(nullptr) + 1);
  EXPECT_EQ(0u, cache.ghosts_->Count());
}

}  // namespace cache
}  // namespace dist_clang
