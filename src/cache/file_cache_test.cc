#include <cache/file_cache.h>

#include <cache/compression.h>

#include <base/c_utils.h>
#include <base/constants.h>
#include <base/file/file.h>
#include <base/future.h>
#include <base/path_utils.h>
#include <base/platform.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>

#include <base/file_utils.h>

#include <gtest/gtest.h>
#include <perf/stat_service.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include STL(regex)
#include STL(future)
#include STL(thread)

namespace dist_clang {
namespace cache {

using namespace string;

namespace {

// A hash of an entry: "<32 hex>-<16 hex>-<16 hex>". It's formed in the code:
// the literals of such forms look like the API keys to the secret scanners.
String EntryHashForTest(ui32 index) {
  char suffix[17];
  snprintf(suffix, sizeof(suffix), "%016x", index);
  return String(32, '1') + "-" + String(16, '2') + "-" + suffix;
}

// The cache checks only the files, that are left before its start.
void MakeOld(const Path& dir) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    struct utimbuf times = {time(nullptr) - 10, time(nullptr) - 10};
    utime(entry.path().c_str(), &times);
  }
}

// The time of the start of a compilation - like the daemon takes it. The files,
// that are modified shortly before or after it, look modified during the
// compilation: the files of the test are modified a bit apart from it.
i64 CompilationTime() {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const i64 now = base::FileSystemTime();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return now;
}

// If the file system of |dir| ignores the case of the names - like the default
// one on Mac.
bool IgnoresCase(const Path& dir) {
  EXPECT_TRUE(base::File::Write(dir / "case", "1"_l));
  const bool result = base::File::Exists(dir / "CASE");
  base::File::Delete(dir / "case");
  return result;
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

TEST(FileCacheTest, KeysOfGeneration) {
  // The keys of the entries are a part of the format of the cache: the older
  // entries wouldn't be found, but would stay in the cache - with their sizes
  // and the statistics. If this test fails, raise |FileCache::kGeneration| and
  // update the keys here: the cache is reset then.
  // The keys are formed in the code: the literals of such forms look like the
  // API keys to the secret scanners.
  const String source_hash = "22f62a6490830d13" + String("ca808c1eeddfc2a2");
  EXPECT_EQ(source_hash + "-" + "f108f3f1b79b8334" + "-" + "717c7b8afebbfb71",
            FileCache::Hash(HandledSource("int main() {}"_l), {},
                            CommandLine("-c"_l), Version("1"_l))
                .str.string_copy());
  EXPECT_EQ(source_hash + "-" + "f108f3f1b79b8334" + "-" + "6369146f18cee2ea",
            FileCache::Hash(UnhandledSource("int main() {}"_l), {},
                            CommandLine("-c"_l), Version("1"_l))
                .str.string_copy());
  EXPECT_EQ(3u, FileCache::kGeneration);
}

TEST(FileCacheTest, HashCompliesWithRegex) {
  // The command line and the compiler have 64 bits each: with 32 bits the
  // collisions are likely in a large cache.
  std::regex hash_regex("[a-f0-9]{32}-[a-f0-9]{16}-[a-f0-9]{16}");
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

TEST(FileCacheTest, OneDaemonPerCache) {
  const base::TemporaryDir temp_dir;
  // Another daemon - e.g. with another socket - would remove the files of the
  // entries, that the first one hasn't indexed yet.
  {
    FileCache first(temp_dir);
    ASSERT_TRUE(first.Run(1));
    FileCache second(temp_dir);
    String error;
    EXPECT_FALSE(second.Run(1, &error));
    // For "dist-clang --stats".
    EXPECT_NE(String::npos, error.find("is used by another daemon, pid " +
                                       std::to_string(getpid())))
        << error;
  }
  // The cache is free again.
  {
    FileCache third(temp_dir);
    EXPECT_TRUE(third.Run(1));
  }

  // The reset of the cache of an old generation keeps the lock.
  proto::Format format;
  format.set_generation(1);
  ASSERT_TRUE(base::SaveToFile(temp_dir.path() / "format", format));
  FileCache fourth(temp_dir);
  ASSERT_TRUE(fourth.Run(1));
  FileCache fifth(temp_dir);
  EXPECT_FALSE(fifth.Run(1));
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
  cache.FlushAccesses();
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

TEST(FileCacheTest, RemoveDirectRowsOfRemovedEntries) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED,
                  proto::NONE, true);
  ASSERT_TRUE(cache.Run(3600));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("int main() {}"_l);
  const auto hash = FileCache::Hash(HandledSource("x"_l), {}, cl, version);
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash,
              String(), {}, CompilationTime());
  cache.FlushAccesses();
  cache.WaitForCleaner();
  const ui64 rows = cache.database_->Count();

  // The rows of the direct cache, that lead to the removed entries, are
  // removed too - otherwise the database only grows.
  ASSERT_TRUE(cache.RemoveEntry(hash));
  std::promise<void> done;
  cache.cleaner_.Push([&] {
    cache.Maintain();
    done.set_value();
  });
  done.get_future().wait();
  EXPECT_EQ(rows - 1, cache.database_->Count());
}

TEST(FileCacheTest, RemoveDirectEntriesOfRemovedEntries) {
  const base::TemporaryDir temp_dir;
  const Path cache_path = temp_dir.path() / "cache";
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code1("int main() {}"_l), code2("int main();"_l);
  const auto hash1 = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  const auto hash2 = FileCache::Hash(HandledSource("2"_l), {}, cl, version);
  const auto direct1 = FileCache::Hash(code1, {}, cl, version);
  const auto direct2 = FileCache::Hash(code2, {}, cl, version);

  auto cache = std::make_unique<FileCache>(cache_path, FileCache::UNLIMITED,
                                           proto::NONE, true);
  ASSERT_TRUE(cache->Run(3600));
  auto Maintain = [&] {
    cache->FlushAccesses();
    cache->WaitForCleaner();
    std::promise<void> done;
    cache->cleaner_.Push([&] {
      cache->Maintain();
      done.set_value();
    });
    done.get_future().wait();
  };
  auto Exists = [&](const UnhandledHash& hash) {
    return base::File::Exists(
        AppendExtension(cache->CommonPath(hash), base::kExtManifest));
  };

  // The first source leads to the first entry, the second one - to both:
  // with different headers.
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache->Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache->Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache->Store(code1, {}, cl, version, {header_path}, {}, temp_dir, hash1,
               String(), {}, CompilationTime());
  cache->Store(code2, {}, cl, version, {header_path}, {}, temp_dir, hash1,
               String(), {}, CompilationTime());
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  cache->Store(code2, {}, cl, version, {header_path}, {}, temp_dir, hash2,
               String(), {}, CompilationTime());
  Maintain();
  ASSERT_TRUE(Exists(direct1));
  ASSERT_TRUE(Exists(direct2));

  // The direct entry, that leads to no entry, finds nothing - it's removed.
  ASSERT_TRUE(cache->RemoveEntry(hash1));
  Maintain();
  EXPECT_FALSE(Exists(direct1));
  EXPECT_TRUE(Exists(direct2));
  FileCache::Entry entry;
  EXPECT_TRUE(cache->Find(code2, {}, cl, version, temp_dir, &entry));

  ASSERT_TRUE(cache->RemoveEntry(hash2));
  Maintain();
  EXPECT_FALSE(Exists(direct2));
  cache.reset();

  // The direct entries of the older revisions don't know their entries: they
  // are removed after the start.
  cache = std::make_unique<FileCache>(cache_path, FileCache::UNLIMITED,
                                      proto::NONE, true);
  ASSERT_TRUE(cache->Run(3600));
  cache->Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache->Store(code1, {}, cl, version, {header_path}, {}, temp_dir, hash1,
               String(), {}, CompilationTime());
  cache->FlushAccesses();
  cache->WaitForCleaner();
  cache.reset();
  const Path manifest_path = AppendExtension(
      FileCache(cache_path).CommonPath(direct1), base::kExtManifest);
  proto::Manifest manifest;
  ASSERT_TRUE(base::LoadBinaryFromFile(manifest_path, &manifest));
  ASSERT_EQ(1, manifest.direct().entries_size());
  manifest.mutable_direct()->clear_entries();
  ASSERT_TRUE(base::SaveBinaryToFile(manifest_path, manifest));
  // The index is rebuilt from the manifests.
  ASSERT_TRUE(base::File::Delete(cache_path / "index.sqlite"));
  MakeOld(cache_path);

  cache = std::make_unique<FileCache>(cache_path, FileCache::UNLIMITED,
                                      proto::NONE, true);
  ASSERT_TRUE(cache->Run(3600));
  cache->WaitForCleaner();
  EXPECT_FALSE(Exists(direct1));
  EXPECT_TRUE(cache->Find(hash1, &entry));
}

TEST(FileCacheTest, DirectEntriesOfTags) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code1("int main() {}"_l), code2("int main();"_l);
  const auto hash1 = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  const auto hash2 = FileCache::Hash(HandledSource("2"_l), {}, cl, version);
  const auto direct1 = FileCache::Hash(code1, {}, cl, version);
  const auto direct2 = FileCache::Hash(code2, {}, cl, version);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true);
  ASSERT_TRUE(cache.Run(3600));

  // The first source leads to the entry of the first tag, the second one - to
  // the entries of both tags.
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.Store(code1, {}, cl, version, {header_path}, {}, temp_dir, hash1,
              String(), {}, CompilationTime());
  cache.Store(code2, {}, cl, version, {header_path}, {}, temp_dir, hash1,
              String(), {}, CompilationTime());
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  cache.Store(code2, {}, cl, version, {header_path}, {}, temp_dir, hash2,
              String(), {}, CompilationTime());
  cache.AddTags(hash1, {"a"});
  cache.AddTags(hash2, {"b"});
  cache.FlushAccesses();
  cache.WaitForCleaner();

  auto EntrySize = [&](const Hash& hash) {
    SQLite::Value value;
    EXPECT_TRUE(cache.entries_->Get(hash.str, &value));
    return std::get<SQLite::SIZE>(value);
  };
  auto BlobSize = [&](const char* content) {
    return cache.blobs_.Size(base::Hexify(Immutable(String(content)).Hash()));
  };
  const ui64 size1 = EntrySize(hash1) + BlobSize("object 1");
  const ui64 direct_size1 = EntrySize(direct1), direct_size2 = EntrySize(direct2);

  // The direct entries count for the tags of their entries.
  List<FileCache::TagStats> stats;
  List<FileCache::TagOverlap> overlaps;
  cache.ComputeTagStats(&stats, &overlaps);
  ASSERT_EQ(2u, stats.size());
  EXPECT_EQ("a", stats.front().tag);
  EXPECT_EQ(1u, stats.front().entries);
  EXPECT_EQ(size1 + direct_size1 + direct_size2, stats.front().size);
  EXPECT_EQ(size1 + direct_size1, stats.front().exclusive_size);
  EXPECT_EQ(size1 + direct_size1, stats.front().freeable_size);
  ASSERT_EQ(1u, overlaps.size());
  EXPECT_EQ(direct_size2, overlaps.front().size);

  // And are removed with them at once.
  const auto result = cache.DoForgetTag("a", false);
  EXPECT_EQ(1u, result.removed_entries);
  EXPECT_EQ(size1 + direct_size1, result.freed_size);
  EXPECT_FALSE(base::File::Exists(
      AppendExtension(cache.CommonPath(direct1), base::kExtManifest)));
  EXPECT_TRUE(base::File::Exists(
      AppendExtension(cache.CommonPath(direct2), base::kExtManifest)));
}

TEST(FileCacheTest, IsolatedTags) {
  const base::TemporaryDir temp_dir;
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash1 = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  const auto hash2 = FileCache::Hash(HandledSource("2"_l), {}, cl, version);
  const auto hash3 = FileCache::Hash(HandledSource("3"_l), {}, cl, version);
  const auto cache_path = temp_dir.path() / "cache";
  FileCache::Entry entry;

  {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::NONE, true);
    cache.SetIsolatedTags({"bench"});
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
    cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
    cache.Store(hash3, FileCache::Entry{"object 3"_l, "deps"_l});
    cache.AddTags(hash2, {"other"});

    // The entries of others are found only outside of the scope of the
    // isolated tag.
    EXPECT_FALSE(cache.Find(hash1, &entry, "bench"));
    EXPECT_FALSE(cache.Find(hash2, &entry, "bench"));
    EXPECT_TRUE(cache.Find(hash1, &entry));

    // The tag counts at once - before the cleaner applies it.
    cache.AddTags(hash1, {"bench", "other"});
    cache.AddTags(hash3, {"bench"});
    EXPECT_TRUE(cache.Find(hash1, &entry, "bench"));
    EXPECT_TRUE(cache.Find(hash3, &entry, "bench"));
    EXPECT_FALSE(cache.Find(hash2, &entry, "bench"));

    // The forgotten tag leaves the scope empty: the shared entry stays for
    // others.
    cache.FlushAccesses();
    cache.WaitForCleaner();
    ASSERT_TRUE(cache.ForgetTag("bench", false, [](const auto&) {}));
    cache.WaitForCleaner();
    EXPECT_FALSE(cache.Find(hash1, &entry, "bench"));
    EXPECT_FALSE(cache.Find(hash3, &entry, "bench"));
    EXPECT_TRUE(cache.Find(hash1, &entry));

    cache.AddTags(hash1, {"bench"});
    cache.Close();
  }

  // The scope stays after the restart - restored by the cleaner, with the
  // tags, that come before it.
  FileCache cache(cache_path, FileCache::UNLIMITED, proto::NONE, true);
  cache.SetIsolatedTags({"bench"});
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  cache.cleaner_.Push([released] { released.wait(); });
  ASSERT_TRUE(cache.Run(3600));
  cache.AddTags(hash2, {"bench"});
  release.set_value();
  cache.WaitForCleaner();
  EXPECT_TRUE(cache.Find(hash1, &entry, "bench"));
  EXPECT_TRUE(cache.Find(hash2, &entry, "bench"));

  // The removed entry leaves it - the same one, stored again by others, isn't
  // in the scope.
  cache.Remove(hash1);
  cache.WaitForCleaner();
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  EXPECT_TRUE(cache.Find(hash1, &entry));
  EXPECT_FALSE(cache.Find(hash1, &entry, "bench"));
}

TEST(FileCacheTest, TagAccessTimes) {
  const base::TemporaryDir temp_dir;
  const auto cache_path = temp_dir.path() / "cache";
  const auto hash = FileCache::Hash(HandledSource("1"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  const String key = hash.str.string_copy();
  ui64 time_a = 0, time_b = 0;

  {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::NONE, true);
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
    cache.AddTags(hash, {"a"});
    cache.FlushAccesses();
    cache.WaitForCleaner();
    time_a = cache.tag_times_["a"][key];
    EXPECT_NE(0u, time_a);

    // Every tag has its own time of the last use of the entry.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    FileCache::Entry entry;
    ASSERT_TRUE(cache.Find(hash, &entry));
    cache.AddTags(hash, {"b"});
    cache.FlushAccesses();
    cache.WaitForCleaner();
    EXPECT_EQ(time_a, cache.tag_times_["a"][key]);
    time_b = cache.tag_times_["b"][key];
    EXPECT_LT(time_a, time_b);
    cache.Close();
  }

  // The tags of the older revisions have no times: they get the time of the
  // last use of the entry.
  {
    TagIndex index(cache_path, "tags");
    ASSERT_TRUE(index.DeleteTime(key, "a"));
  }
  FileCache cache(cache_path, FileCache::UNLIMITED, proto::NONE, true);
  ASSERT_TRUE(cache.Run(3600));
  cache.WaitForCleaner();
  SQLite::Value value;
  ASSERT_TRUE(cache.entries_->Get(key, &value));
  EXPECT_EQ(std::get<SQLite::MTIME>(value), cache.tag_times_["a"][key]);
  EXPECT_EQ(time_b, cache.tag_times_["b"][key]);
  EXPECT_EQ(time_b, TagIndex(cache_path, "tags").GetAllTimes()["b"][key]);

  // The forgotten tag forgets its times.
  ASSERT_TRUE(cache.ForgetTag("a", false, [](const auto&) {}));
  cache.WaitForCleaner();
  EXPECT_FALSE(cache.tag_times_["a"].contains(key));
  EXPECT_FALSE(TagIndex(cache_path, "tags").GetAllTimes()["a"].contains(key));
}

TEST(FileCacheTest, TagQuotas) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("1"_l);
  const UnhandledSource code("#include \"test.h\""_l);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true);
  cache.SetIsolatedTags({"q"});
  ASSERT_TRUE(cache.Run(3600));

  Vector<HandledHash> hashes;
  for (int i = 0; i < 3; ++i) {
    hashes.push_back(FileCache::Hash(HandledSource(Immutable(std::to_string(i))),
                                     {}, cl, version));
    cache.Store(hashes[i], FileCache::Entry{Immutable(String(1000, '0' + i)),
                                            "deps"_l});
    cache.AddTags(hashes[i], {"q"});
  }
  cache.AddTags(hashes[0], {"other"});
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, hashes[2],
              String(), {}, CompilationTime(), "q");
  cache.FlushAccesses();
  cache.WaitForCleaner();

  auto Key = [&](int i) { return hashes[i].str.string_copy(); };
  auto IndexSize = [&](const String& key) {
    SQLite::Value value;
    EXPECT_TRUE(cache.entries_->Get(key, &value));
    return std::get<SQLite::SIZE>(value);
  };
  // The entries have the same sizes - with their objects.
  const ui64 size = IndexSize(Key(0)) + 1000;
  const ui64 direct_size =
      IndexSize(FileCache::Hash(code, {}, cl, version).str.string_copy());
  auto HasTag = [&](int i) {
    auto it = cache.entry_tags_.find(Key(i));
    return it != cache.entry_tags_.end() &&
           std::find(it->second.begin(), it->second.end(), "q") !=
               it->second.end();
  };

  // The oldest entries by the uses of the tag lose it - the hits of others
  // don't count. The entry stays for others.
  cache.tag_times_["q"][Key(0)] = 100;
  cache.tag_times_["q"][Key(1)] = 200;
  cache.tag_times_["q"][Key(2)] = 300;
  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hashes[0], &entry));
  cache.SetTagQuotas({{"q", size * 2 + size / 2}});
  cache.TrimQuotas();
  EXPECT_FALSE(HasTag(0));
  EXPECT_TRUE(HasTag(1));
  EXPECT_TRUE(HasTag(2));
  EXPECT_FALSE(cache.tag_times_["q"].contains(Key(0)));
  EXPECT_FALSE(cache.Find(hashes[0], &entry, "q"));
  EXPECT_TRUE(cache.Find(hashes[0], &entry));

  // The direct entries of the tag count too. The entry without tags stays
  // for the common rules.
  cache.SetTagQuotas({{"q", size * 2 + direct_size - 1}});
  cache.TrimQuotas();
  EXPECT_FALSE(HasTag(1));
  EXPECT_TRUE(HasTag(2));
  EXPECT_FALSE(cache.entry_tags_.contains(Key(1)));
  EXPECT_TRUE(cache.Find(hashes[1], &entry));
  EXPECT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(), {},
                         "q"));

  // The index remembers it.
  const auto tags = cache.tag_index_->GetAll();
  EXPECT_EQ(Vector<String>{"other"}, tags.at(Key(0)));
  EXPECT_FALSE(tags.contains(Key(1)));
  EXPECT_EQ(Vector<String>{"q"}, tags.at(Key(2)));
  auto times = cache.tag_index_->GetAllTimes();
  EXPECT_FALSE(times["q"].contains(Key(0)));
  EXPECT_FALSE(times["q"].contains(Key(1)));
  EXPECT_TRUE(times["q"].contains(Key(2)));
  EXPECT_TRUE(times["other"].contains(Key(0)));

  // The unlimited quota.
  cache.SetTagQuotas({{"q", 0}});
  cache.TrimQuotas();
  EXPECT_TRUE(HasTag(2));
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
      AppendExtension(cache.CommonPath(hash), base::kExtManifest)));

  // The blob without references is removed too - by the cleaner.
  const String blob = base::Hexify(entry.object.Hash());
  bool removed = false;
  for (int i = 0; i < 100 && !removed; ++i) {
    removed = !base::File::Exists(cache.blobs_.RawPath(blob)) &&
              !base::File::Exists(cache.blobs_.PackedPath(blob));
    if (!removed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  EXPECT_TRUE(removed);
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

TEST(CompressionTest, BrokenFrameSize) {
  String packed;
  ASSERT_TRUE(Pack(proto::ZSTD, Immutable(String(1000, 'a')), &packed));
  String unpacked;
  ASSERT_TRUE(Unpack(proto::ZSTD, Immutable(String(packed)), &unpacked));
  EXPECT_EQ(String(1000, 'a'), unpacked);

  // The size of the contents in the header of the frame is broken: it's much
  // larger than the frame can give. After the magic number goes the
  // descriptor of the frame, then the window, the dictionary and the size.
  String broken = packed;
  const ui8 descriptor = broken[4];
  const bool single_segment = descriptor & 0x20;
  const size_t dictionary_sizes[] = {0, 1, 2, 4};
  const size_t size_sizes[] = {single_segment ? 1u : 0u, 2, 4, 8};
  const size_t position =
      5 + (single_segment ? 0 : 1) + dictionary_sizes[descriptor & 3];
  String size(8, '\0');
  size[7] = '\x40';
  broken.replace(position, size_sizes[descriptor >> 6], size);
  broken[4] = static_cast<char>(descriptor | 0xc0);
  EXPECT_FALSE(Unpack(proto::ZSTD, Immutable(String(broken)), &unpacked));
}

TEST(CompressionTest, BrokenFrames) {
  const String contents(100000, 'a');
  String packed;
  ASSERT_TRUE(Pack(proto::ZSTD, Immutable(String(contents)), &packed));
  String unpacked;

  EXPECT_FALSE(Unpack(proto::ZSTD, Immutable(String()), &unpacked));
  EXPECT_FALSE(Unpack(proto::ZSTD, Immutable(packed.substr(0, 3)), &unpacked));

  // The frame ends early - at any point.
  for (size_t size = 4; size < packed.size(); ++size) {
    EXPECT_FALSE(
        Unpack(proto::ZSTD, Immutable(packed.substr(0, size)), &unpacked))
        << size;
  }

  // The size in the header is smaller than the contents.
  String smaller;
  ASSERT_TRUE(Pack(proto::ZSTD, Immutable(String(1000, 'a')), &smaller));
  const ui8 descriptor = smaller[4];
  ASSERT_EQ(0x20, descriptor & 0x20) << "a single segment is expected";
  ASSERT_EQ(1, descriptor >> 6) << "the size of two bytes is expected";
  const size_t position = 5 + (descriptor & 3 ? 1 << ((descriptor & 3) - 1)
                                              : 0);
  // The sizes of two bytes are stored minus 256.
  smaller[position] = 0;
  smaller[position + 1] = 0;
  EXPECT_FALSE(Unpack(proto::ZSTD, Immutable(String(smaller)), &unpacked));

  ASSERT_TRUE(Unpack(proto::ZSTD, Immutable(String(packed)), &unpacked));
  EXPECT_EQ(contents, unpacked);
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

  // The blobs are really compressed.
  const String blob = base::Hexify(Immutable(String(object)).Hash());
  EXPECT_FALSE(base::File::Exists(cache.blobs_.RawPath(blob)));
  ASSERT_TRUE(base::File::Exists(cache.blobs_.PackedPath(blob)));
  EXPECT_GT(object.size() / 4,
            base::File::Size(cache.blobs_.PackedPath(blob)))
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
  EXPECT_TRUE(st.st_mode & S_IWUSR);

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
  cache.FlushAccesses();
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

  // The used entry is uncompressed again - to be linked.
  FileCache::Entry found;
  ASSERT_TRUE(cache.Find(hash, &found));
  EXPECT_EQ(cache.blobs_.RawPath(object_hash), found.object_path);
  EXPECT_EQ(cache.blobs_.RawPath(dwo_hash), found.dwo_path);
  Immutable object_content, dwo_content;
  ASSERT_TRUE(base::File::Read(found.object_path, &object_content));
  EXPECT_EQ(object, object_content.string_copy());
  ASSERT_TRUE(base::File::Read(found.dwo_path, &dwo_content));
  EXPECT_EQ(dwo, dwo_content.string_copy());
  EXPECT_EQ("deps"_l, found.deps);

  // The compressed blobs are removed by the cleaner.
  cache.FlushAccesses();
  for (int i = 0;
       i < 300 && base::File::Exists(cache.blobs_.PackedPath(object_hash));
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(base::File::Exists(cache.blobs_.PackedPath(object_hash)));
  EXPECT_FALSE(base::File::Exists(cache.blobs_.PackedPath(dwo_hash)));
  ASSERT_TRUE(cache.Find(hash, &found));
  EXPECT_EQ(cache.blobs_.RawPath(object_hash), found.object_path);
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
    cache.FlushAccesses();
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
  // The sizes are reported by the cleaner only.
  auto ReportSizes = [&cache] {
    std::promise<void> done;
    cache.cleaner_.Push([&] {
      cache.ReportSizes();
      done.set_value();
    });
    done.get_future().wait();
  };
  ReportSizes();
  EXPECT_EQ(object.size(), GetStat(M::DISK_SAVED_BY_DEDUPLICATION));
  EXPECT_EQ(0u, GetStat(M::DISK_SAVED_BY_LINKS));
  ASSERT_TRUE(base::File::Delete(object_path));
  ASSERT_EQ(0, link(blob_path.c_str(), object_path.c_str()));
  ReportSizes();
  EXPECT_EQ(0u, GetStat(M::DISK_SAVED_BY_LINKS));
  EXPECT_EQ(object.size(), GetStat(M::CACHE_SHARED_SIZE));
  const Path second_path = temp_dir.path() / "second.o";
  ASSERT_EQ(0, link(blob_path.c_str(), second_path.c_str()));
  ReportSizes();
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

TEST(FileCacheTest, KeepBlobsOfHitsDuringScan) {
  const base::TemporaryDir temp_dir;
  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  Path blob_path;
  {
    FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
    ASSERT_TRUE(cache.Run(3600));
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
    blob_path = cache.blobs_.PackedPath(
        base::Hexify(Immutable("object"_l).Hash()));
  }
  ASSERT_TRUE(base::File::Exists(blob_path));

  // The index is lost in a crash: the entry is indexed again by the scan.
  ASSERT_TRUE(base::File::Delete(temp_dir.path() / "index.sqlite"));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "running", "1"_l));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // The entry is hit during the scan: the scan skips it, as a new one.
  FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
  std::promise<void> hit;
  cache.cleaner_.Push([future = hit.get_future().share()] { future.wait(); });
  ASSERT_TRUE(cache.Run(3600));
  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  const auto manifest_path =
      AppendExtension(cache.CommonPath(hash), base::kExtManifest);
  const struct utimbuf scan_time = {time(nullptr) + 2, time(nullptr) + 2};
  ASSERT_EQ(0, utime(manifest_path.c_str(), &scan_time));
  hit.set_value();
  cache.WaitForCleaner();

  EXPECT_TRUE(base::File::Exists(blob_path));
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

TEST(FileCacheTest, RemoveBlobsOfFailedStore) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
  ASSERT_TRUE(cache.Run(3600));
  const auto hash = TestHash("-c");
  const String object_hash = base::Hexify(Immutable("object"_l).Hash());

  // The dependencies can't be written: nothing refers to the new blob.
  const auto deps_path = AppendExtension(cache.CommonPath(hash), base::kExtDeps);
  ASSERT_TRUE(base::CreateDirectory(deps_path));
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(hash, &entry));
  EXPECT_FALSE(base::File::Exists(cache.blobs_.PackedPath(object_hash)));

  // The reused blob stays.
  const auto other_hash = TestHash("-c -O1");
  cache.Store(other_hash, FileCache::Entry{"object"_l, "deps"_l});
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  EXPECT_TRUE(base::File::Exists(cache.blobs_.PackedPath(object_hash)));
  EXPECT_TRUE(cache.Find(other_hash, &entry));
}

TEST(FileCacheTest, TimeTraces) {
  using M = perf::proto::Metric;
  const base::TemporaryDir temp_dir;
  const auto hash = TestHash("-c");
  const auto other_hash = TestHash("-c -O1");
  const String trace_hash = base::Hexify(Immutable("trace"_l).Hash());
  auto Open = [&](bool store_time_traces) {
    auto cache = std::make_unique<FileCache>(
        temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true, true);
    cache->SetStoreTimeTraces(store_time_traces);
    EXPECT_TRUE(cache->Run(3600));
    cache->WaitForCleaner();
    return cache;
  };
  // The outputs are linked.
  auto Object = [](const FileCache::Entry& entry) {
    Immutable object;
    EXPECT_TRUE(base::File::Read(entry.object_path, &object));
    return object;
  };
  auto BlobExists = [](const FileCache& cache, const String& hash) {
    return base::File::Exists(cache.blobs_.RawPath(hash)) ||
           base::File::Exists(cache.blobs_.PackedPath(hash));
  };

  {
    auto cache = Open(true);
    FileCache::Entry stored{"object"_l, "deps"_l};
    stored.time_trace = "trace"_l;
    stored.time_trace_options = "options";
    cache->Store(hash, stored);
    cache->Store(other_hash, FileCache::Entry{"object2"_l, "deps"_l});

    // The trace is never linked: it's compressed at once.
    EXPECT_TRUE(base::File::Exists(cache->blobs_.PackedPath(trace_hash)));
    EXPECT_FALSE(base::File::Exists(cache->blobs_.RawPath(trace_hash)));

    // Only the hash of the trace: it's read, if it's needed.
    FileCache::Entry entry;
    ASSERT_TRUE(cache->Find(hash, &entry));
    EXPECT_TRUE(entry.time_trace.empty());
    EXPECT_EQ(trace_hash, entry.time_trace_hash);
    EXPECT_EQ("options", entry.time_trace_options);
    Immutable trace;
    ASSERT_TRUE(cache->FindTimeTrace(entry.time_trace_hash, &trace));
    EXPECT_EQ("trace"_l, trace);

    ASSERT_TRUE(cache->Find(other_hash, &entry));
    EXPECT_TRUE(entry.time_trace_hash.empty());
    EXPECT_TRUE(entry.time_trace_options.empty());
    cache->FlushAccesses();
    cache->WaitForCleaner();
  }

  // The size of the traces is known after the start - from the index.
  {
    auto cache = Open(true);
    EXPECT_LT(0u, GetStat(M::CACHE_TIME_TRACES_SIZE));
    EXPECT_EQ(cache->blobs_.Size(trace_hash),
              GetStat(M::CACHE_TIME_TRACES_SIZE));
    FileCache::Entry entry;
    ASSERT_TRUE(cache->Find(hash, &entry));
    EXPECT_EQ(trace_hash, entry.time_trace_hash);
  }

  // Without the option the traces aren't stored, and the stored ones are
  // removed - the entries stay.
  {
    auto cache = Open(false);
    EXPECT_FALSE(BlobExists(*cache, trace_hash));
    EXPECT_EQ(0u, GetStat(M::CACHE_TIME_TRACES_SIZE));
    FileCache::Entry entry;
    ASSERT_TRUE(cache->Find(hash, &entry));
    EXPECT_EQ("object"_l, Object(entry));
    EXPECT_TRUE(entry.time_trace_hash.empty());

    FileCache::Entry stored{"object2"_l, "deps"_l};
    stored.time_trace = "trace"_l;
    stored.time_trace_options = "options";
    cache->Store(other_hash, stored);
    ASSERT_TRUE(cache->Find(other_hash, &entry));
    EXPECT_TRUE(entry.time_trace_hash.empty());
    EXPECT_FALSE(BlobExists(*cache, trace_hash));
  }

  // The lost trace doesn't lose the entry - e.g. after an older daemon, that
  // doesn't count the references to the traces.
  {
    auto cache = Open(true);
    FileCache::Entry stored{"object"_l, "deps"_l};
    stored.time_trace = "trace"_l;
    cache->Store(hash, stored);
    ASSERT_TRUE(BlobExists(*cache, trace_hash));
    base::File::Delete(cache->blobs_.RawPath(trace_hash));
    base::File::Delete(cache->blobs_.PackedPath(trace_hash));
    FileCache::Entry entry;
    ASSERT_TRUE(cache->Find(hash, &entry));
    EXPECT_EQ("object"_l, Object(entry));
    Immutable trace;
    EXPECT_FALSE(cache->FindTimeTrace(entry.time_trace_hash, &trace));
  }
}

TEST(FileCacheTest, RemoveLockedEntry) {
  // The wrong entry is removed, though it's being read at the moment.
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir.path(), FileCache::UNLIMITED, proto::ZSTD, true);
  ASSERT_TRUE(cache.Run(3600));
  const auto hash = TestHash("-c");
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  const auto manifest_path =
      AppendExtension(cache.CommonPath(hash), base::kExtManifest);

  {
    FileCache::ReadLock lock(&cache, manifest_path);
    ASSERT_TRUE(lock);
    cache.Remove(hash);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  for (int i = 0; i < 300 && base::File::Exists(manifest_path); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(hash, &entry));
}

TEST(FileCacheTest, TagStatsOfOwnRules) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
  cache.SetIsolatedTags({"a"});
  cache.SetTagQuotas({{"a", 1000}, {"c", 0}});
  ASSERT_TRUE(cache.Run(3600));

  const auto hash = TestHash("-c");
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  cache.AddTags(hash, {"a", "b"});
  cache.FlushAccesses();
  cache.WaitForCleaner();

  List<FileCache::TagStats> stats;
  List<FileCache::TagOverlap> overlaps;
  cache.ComputeTagStats(&stats, &overlaps);
  // The tags with own sizes are shown without entries too.
  ASSERT_EQ(3u, stats.size());
  auto it = stats.begin();
  EXPECT_EQ("a", it->tag);
  EXPECT_TRUE(it->isolated);
  EXPECT_EQ(std::optional<ui64>(1000), it->quota);
  ++it;
  EXPECT_EQ("b", it->tag);
  EXPECT_FALSE(it->isolated);
  EXPECT_FALSE(it->quota);
  ++it;
  EXPECT_EQ("c", it->tag);
  EXPECT_EQ(0u, it->entries);
  EXPECT_EQ(std::optional<ui64>(0), it->quota);
}

TEST(FileCacheTest, TagStats) {
  const base::TemporaryDir temp_dir;
  // Makes the cleaner process the stored entries and the tags.
  auto FlushToIndex = [](FileCache& cache, const List<HandledHash>& hashes) {
    cache.FlushAccesses();
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

TEST(FileCacheTest, FindDoesntKeepOldFields) {
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::ZSTD, false);
  ASSERT_TRUE(cache.Run(1));

  const auto hash1 = TestHash("-c");
  const auto hash2 = TestHash("-c -O2");
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps 1"_l, "stderr 1"_l});
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps 2"_l});

  // E.g. a failed lookup of another entry - in the direct cache - has filled
  // some fields: the compressed object comes without a path.
  FileCache::Entry entry;
  entry.object_path = temp_dir.path() / "other.o";
  entry.stderr = "other stderr"_l;
  ASSERT_TRUE(cache.Find(hash2, &entry));
  EXPECT_EQ("object 2"_l, entry.object);
  EXPECT_TRUE(entry.object_path.empty());
  EXPECT_TRUE(entry.stderr.empty());

  // A failed lookup leaves the entry untouched.
  ASSERT_TRUE(base::File::Delete(
      AppendExtension(cache.CommonPath(hash1), base::kExtDeps)));
  FileCache::Entry failed;
  EXPECT_FALSE(cache.Find(hash1, &failed));
  EXPECT_TRUE(failed.object.empty());
  EXPECT_TRUE(failed.stderr.empty());
}

TEST(FileCacheTest, BinaryManifests) {
  // The manifests are read on every lookup: the text format takes much more
  // time to parse.
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  const auto hash = TestHash("-c");
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});

  Immutable contents;
  ASSERT_TRUE(base::File::Read(
      AppendExtension(cache.CommonPath(hash), base::kExtManifest), &contents));
  proto::Manifest manifest;
  EXPECT_TRUE(manifest.ParseFromString(contents.string_copy()));
  EXPECT_TRUE(manifest.has_v1());
  EXPECT_FALSE(contents.string_copy().starts_with("version:"));
}

TEST(FileCacheTest, AccessesWhileFlushed) {
  // The accesses are appended by the workers, while the cleaner takes them.
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(3600));
  const auto hash = TestHash("-c");
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});

  Atomic<bool> done = false;
  Vector<std::thread> workers;
  for (int i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      while (!done) {
        FileCache::Entry entry;
        EXPECT_TRUE(cache.Find(hash, &entry));
      }
    });
  }
  for (int i = 0; i < 2000; ++i) {
    cache.FlushAccesses();
  }
  done = true;
  for (auto& worker : workers) {
    worker.join();
  }
}

TEST(FileCacheTest, ForgetTag) {
  const base::TemporaryDir temp_dir;
  // Makes the cleaner process the stored entries and the tags.
  auto FlushToIndex = [](FileCache& cache, const List<HandledHash>& hashes) {
    cache.FlushAccesses();
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
  String error;
  auto Found = [&] {
    FileCache cache(cache_path, FileCache::UNLIMITED, proto::ZSTD, true);
    error.clear();
    if (!cache.Run(3600, &error)) {
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

  // The newer revision may add the fields, that this daemon doesn't know.
  ASSERT_TRUE(base::File::Write(
      format_path, Immutable(String("generation: ") +
                             std::to_string(FileCache::kGeneration) +
                             "\nrevision: " +
                             std::to_string(FileCache::kRevision + 1) +
                             "\nfuture_field: 1\nfuture_message { a: 1 }\n")));
  EXPECT_TRUE(Found());

  // The newer generation isn't used - and isn't touched.
  WriteFormat(FileCache::kGeneration + 1, 0);
  EXPECT_FALSE(Found());
  EXPECT_NE(String::npos,
            error.find("has the generation " +
                       std::to_string(FileCache::kGeneration + 1)))
      << error;
  EXPECT_EQ(FileCache::kGeneration + 1, ReadFormat().generation());
  WriteFormat(FileCache::kGeneration, FileCache::kRevision);
  EXPECT_TRUE(Found());

  // The unreadable format can't tell anything: the cache isn't used - and
  // isn't touched.
  ASSERT_TRUE(base::File::Write(format_path, "broken {"_l));
  EXPECT_FALSE(Found());
  EXPECT_NE(String::npos, error.find("Can't read the format")) << error;
  Immutable contents;
  ASSERT_TRUE(base::File::Read(format_path, &contents));
  EXPECT_EQ("broken {"_l, contents);
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
  ASSERT_TRUE(base::LoadBinaryFromFile(manifest_path, &manifest));
  manifest.mutable_v1()->clear_object_hash();
  ASSERT_TRUE(base::SaveBinaryToFile(manifest_path, manifest));

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

  const auto hash = FileCache::Hash(HandledSource("int main() {}"_l), {},
                                    CommandLine("-c"_l), Version("1"_l));
  {
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

    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, "stderr"_l});
  }

  // The complete entries are kept - after a crash too.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "running", "1"_l));
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

  ASSERT_TRUE(cache.Run(3600));
  auto db_size = cache.MetadataSize();
  auto Flush = [&cache] {
    cache.FlushAccesses();
    cache.WaitForCleaner();
  };
  auto Maintain = [&cache] {
    cache.FlushAccesses();
    cache.cleaner_.Push([&cache] { cache.Maintain(); });
    cache.WaitForCleaner();
  };
  // The times of the accesses are in seconds - the order is set explicitly.
  auto SetTime = [&](int i, ui64 time) {
    const String hash =
        FileCache::Hash(code[i], {}, cl, version).str.string_copy();
    cache.cleaner_.Push([&cache, hash, time] {
      SQLite::Value value;
      ASSERT_TRUE(cache.entries_->Get(hash, &value));
      std::get<SQLite::MTIME>(value) = time;
      ASSERT_TRUE(cache.entries_->Set(hash, value));
    });
    cache.WaitForCleaner();
  };
  const ui64 now = time(nullptr);

  {
    FileCache::Entry entry{obj_content[0], String(), String()};
    cache.Store(FileCache::Hash(code[0], {}, cl, version), entry);
    EXPECT_EQ(entry_size[0],
              base::CalculateDirectorySize(cache_path) - db_size);
  }

  {
    FileCache::Entry entry{obj_content[1], String(), String()};
    cache.Store(FileCache::Hash(code[1], {}, cl, version), entry);
    EXPECT_EQ(entry_size[0] + entry_size[1],
              base::CalculateDirectorySize(cache_path) - db_size);
  }
  Flush();
  SetTime(0, now - 30);
  SetTime(1, now - 20);

  {
    FileCache::Entry entry{obj_content[2], String(), String()};
    cache.Store(FileCache::Hash(code[2], {}, cl, version), entry);
    Maintain();
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
  ASSERT_TRUE(cache.Run(3600));
  auto Flush = [&cache] {
    cache.FlushAccesses();
    cache.WaitForCleaner();
  };
  auto Maintain = [&cache] {
    cache.FlushAccesses();
    cache.cleaner_.Push([&cache] { cache.Maintain(); });
    cache.WaitForCleaner();
  };
  // The times of the accesses are in seconds - the order is set explicitly.
  auto SetTime = [&](int i, ui64 time) {
    const String hash = Hash(i).str.string_copy();
    cache.cleaner_.Push([&cache, hash, time] {
      SQLite::Value value;
      ASSERT_TRUE(cache.entries_->Get(hash, &value));
      std::get<SQLite::MTIME>(value) = time;
      ASSERT_TRUE(cache.entries_->Set(hash, value));
    });
    cache.WaitForCleaner();
  };
  const ui64 now = time(nullptr);

  // The first entry is used again - the second one is removed, though it's
  // newer.
  cache.Store(Hash(0), FileCache::Entry{obj_content[0], String(), String()});
  cache.Store(Hash(1), FileCache::Entry{obj_content[1], String(), String()});
  FileCache::Entry entry0;
  ASSERT_TRUE(cache.Find(Hash(0), &entry0));
  Flush();
  SetTime(0, now - 30);
  SetTime(1, now - 20);
  cache.Store(Hash(2), FileCache::Entry{obj_content[2], String(), String()});
  Maintain();

  FileCache::Entry entry1, entry2, entry3;
  EXPECT_TRUE(cache.Find(Hash(0), &entry1));
  EXPECT_FALSE(cache.Find(Hash(1), &entry2));
  EXPECT_TRUE(cache.Find(Hash(2), &entry3));
}

TEST(FileCacheTest, KeepEntriesOfIsolatedTags) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code("#include \"test.h\""_l);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::ZSTD,
                  true, true);
  cache.SetIsolatedTags({"bench"});
  cache.SetCompressAfter(1);
  ASSERT_TRUE(cache.Run(3600));

  // The outputs aren't linked with the build directory anymore.
  auto Store = [&](const String& content) {
    const auto object_path = temp_dir.path() / "test.o";
    EXPECT_TRUE(base::File::Write(object_path, Immutable(content)));
    const auto hash = FileCache::Hash(HandledSource(Immutable(content)), {},
                                      cl, version);
    FileCache::Entry entry;
    entry.object_path = object_path;
    entry.deps = "deps"_l;
    cache.Store(hash, entry);
    EXPECT_TRUE(base::File::Delete(object_path));
    return hash;
  };
  const String bench_object(100000, 'b'), other_object(100000, 'o');
  const auto bench_hash = Store(bench_object);
  const auto other_hash = Store(other_object);
  cache.AddTags(bench_hash, {"bench"});
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, bench_hash,
              String(), {}, CompilationTime(), "bench");
  cache.FlushAccesses();
  cache.WaitForCleaner();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Only the outputs of others are compressed.
  const String bench_blob = base::Hexify(Immutable(String(bench_object)).Hash());
  const String other_blob = base::Hexify(Immutable(String(other_object)).Hash());
  EXPECT_EQ(1u, cache.CompressColdEntries());
  EXPECT_TRUE(base::File::Exists(cache.blobs_.RawPath(bench_blob)));
  EXPECT_FALSE(base::File::Exists(cache.blobs_.RawPath(other_blob)));

  // Only the entries of others are evicted, if the isolated tag has its own
  // size - the direct entry of the isolated tag stays too.
  cache.SetTagQuotas({{"bench", 0}});
  cache.max_size_ = 1;
  cache.Maintain();
  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(other_hash, &entry));
  EXPECT_TRUE(cache.Find(bench_hash, &entry, "bench"));
  EXPECT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(), {},
                         "bench"));
}

TEST(FileCacheTest, HoldEntriesOfTagsWithOwnSizes) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code("#include \"test.h\""_l);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true, true);
  cache.SetIsolatedTags({"iso"});
  cache.SetTagQuotas({{"held", 0}});
  ASSERT_TRUE(cache.Run(3600));

  // The outputs aren't linked with the build directory anymore.
  auto Store = [&](const String& content) {
    const auto object_path = temp_dir.path() / "test.o";
    EXPECT_TRUE(base::File::Write(object_path, Immutable(content)));
    const auto hash = FileCache::Hash(HandledSource(Immutable(content)), {},
                                      cl, version);
    FileCache::Entry entry;
    entry.object_path = object_path;
    entry.deps = "deps"_l;
    cache.Store(hash, entry);
    EXPECT_TRUE(base::File::Delete(object_path));
    return hash;
  };
  const auto held_hash = Store(String(1000000, 'h'));
  const auto iso_hash = Store(String(1000, 'i'));
  const auto other_hash = Store(String(1000, 'o'));
  cache.AddTags(held_hash, {"held"});
  cache.AddTags(iso_hash, {"iso"});
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, held_hash);
  // The held entry goes last in the order of the eviction.
  FileCache::Entry entry;
  EXPECT_TRUE(cache.Find(held_hash, &entry));
  cache.FlushAccesses();
  cache.WaitForCleaner();

  // The held entries don't count in the size of the cache.
  cache.max_size_ = 100000;
  cache.Maintain();
  EXPECT_TRUE(cache.Find(other_hash, &entry));
  EXPECT_TRUE(cache.Find(iso_hash, &entry, "iso"));

  // The held entries aren't evicted - with their direct entries. The isolated
  // ones without own sizes follow the common rules.
  cache.max_size_ = 1;
  cache.Maintain();
  EXPECT_FALSE(cache.Find(other_hash, &entry));
  EXPECT_FALSE(cache.Find(iso_hash, &entry, "iso"));
  EXPECT_TRUE(cache.Find(held_hash, &entry));
  EXPECT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry));
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
  ASSERT_TRUE(cache.Run(3600));
  auto Maintain = [&cache] {
    cache.FlushAccesses();
    cache.cleaner_.Push([&cache] { cache.Maintain(); });
    cache.WaitForCleaner();
  };

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
  Maintain();

  FileCache::Entry entry0, entry1;
  EXPECT_TRUE(cache.Find(Hash(0), &entry0));
  EXPECT_FALSE(cache.Find(Hash(1), &entry1));

  // The link is gone - the entry isn't protected anymore.
  ASSERT_TRUE(base::File::Delete(object_path));
  cache.Store(Hash(2), FileCache::Entry{"object2"_l, String(), String()});
  Maintain();

  FileCache::Entry entry2, entry3;
  EXPECT_FALSE(cache.Find(Hash(0), &entry2));
  EXPECT_FALSE(cache.Find(Hash(2), &entry3));
}

TEST(FileCacheTest, KeepDirectEntriesOfLinkedOnes) {
  // The direct entries of the protected ones aren't removed for the free
  // space of the disk: they're small, and the protected ones are found by
  // them without the preprocessing.
  const base::TemporaryDir temp_dir;
  const auto object_path = temp_dir.path() / "test.o";
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash =
      FileCache::Hash(HandledSource("int main() { return 0; }"_l), {}, cl,
                      version);
  const UnhandledSource orig_code("int main() {}"_l);

  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  false, true);
  cache.SetMinFreePercent(100);
  ASSERT_TRUE(cache.Run(3600));

  ASSERT_TRUE(base::File::Write(object_path, "object"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  FileCache::Entry linked;
  linked.object_path = object_path;
  cache.Store(hash, linked);
  struct stat st;
  ASSERT_EQ(0, stat(object_path.c_str(), &st));
  if (st.st_nlink < 2) {
    // The filesystem doesn't support hard links - nothing to check.
    return;
  }
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash);
  cache.FlushAccesses();
  cache.cleaner_.Push([&cache] { cache.Maintain(); });
  cache.WaitForCleaner();

  FileCache::Entry entry;
  EXPECT_TRUE(cache.Find(hash, &entry));
  EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
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

TEST(FileCacheTest, DirectEntry_IsolatedTags) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code("#include \"test.h\""_l);
  const auto hash1 = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  const auto hash2 = FileCache::Hash(HandledSource("2"_l), {}, cl, version);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true);
  cache.SetIsolatedTags({"bench"});
  ASSERT_TRUE(cache.Run(3600));
  FileCache::Entry entry;

  // The entry of the isolated tag - e.g. its simple hit - is found by the
  // direct entry of others only outside of the scope.
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(hash1, FileCache::Entry{"object 1"_l, "deps"_l});
  cache.AddTags(hash1, {"bench"});
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, hash1,
              String(), {}, CompilationTime());
  EXPECT_FALSE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(),
                          {}, "bench"));
  EXPECT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry));

  // Its own direct entry is found in the scope.
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, hash1,
              String(), {}, CompilationTime(), "bench");
  ASSERT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(),
                         {}, "bench"));
  EXPECT_EQ(hash1.str.string_copy(), entry.handled_hash);

  // The same headers with other contents - stored by others - lead to the
  // entry of others: not in the scope. The variant of the headers stays in it.
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  cache.Store(hash2, FileCache::Entry{"object 2"_l, "deps"_l});
  cache.AddTags(hash2, {"bench"});
  cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, hash2,
              String(), {}, CompilationTime());
  EXPECT_FALSE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(),
                          {}, "bench"));
  ASSERT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry));
  EXPECT_EQ(hash2.str.string_copy(), entry.handled_hash);

  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  ASSERT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(),
                         {}, "bench"));
  EXPECT_EQ(hash1.str.string_copy(), entry.handled_hash);
}

TEST(FileCacheTest, DirectEntry_IsolatedTagsKeepVariants) {
  const base::TemporaryDir temp_dir;
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource code("#include \"test.h\""_l);
  const auto direct = FileCache::Hash(code, {}, cl, version);
  FileCache cache(temp_dir.path() / "cache", FileCache::UNLIMITED, proto::NONE,
                  true);
  cache.SetIsolatedTags({"bench"});
  ASSERT_TRUE(cache.Run(3600));

  auto Store = [&](const String& header, const String& content,
                   const String& scope) {
    const auto header_path = temp_dir.path() / header;
    ASSERT_TRUE(base::File::Write(header_path, Immutable(content)));
    const auto hash = FileCache::Hash(HandledSource(Immutable(content)), {},
                                      cl, version);
    cache.Store(hash, FileCache::Entry{Immutable(content), "deps"_l});
    if (!scope.empty()) {
      cache.AddTags(hash, {scope});
    }
    cache.Store(code, {}, cl, version, {header_path}, {}, temp_dir, hash,
                String(), {}, CompilationTime(), scope);
  };

  // The variant and the entry of the isolated tag stay after many variants
  // and entries of others - and don't take their places.
  Store("bench.h", "bench", "bench");
  for (ui32 i = 0; i < FileCache::kMaxDirectVariants; ++i) {
    Store("other" + std::to_string(i) + ".h", "variant " + std::to_string(i),
          String());
  }
  for (ui32 i = 0; i < FileCache::kMaxDirectEntries; ++i) {
    Store("other0.h", "entry " + std::to_string(i), String());
  }

  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(code, {}, cl, version, temp_dir, &entry, String(), {},
                         "bench"));
  EXPECT_EQ("bench", entry.object.string_copy());

  proto::Manifest manifest;
  ASSERT_TRUE(base::LoadBinaryFromFile(
      AppendExtension(cache.CommonPath(direct), base::kExtManifest),
      &manifest));
  EXPECT_EQ(int(FileCache::kMaxDirectVariants) + 1,
            manifest.direct().variants_size());
  EXPECT_EQ(int(FileCache::kMaxDirectEntries) + 1,
            manifest.direct().entries_size());
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

TEST(FileCacheTest, DirectEntry_TimeMacrosInFlags) {
  const base::TemporaryDir temp_dir;
  const auto header_path = temp_dir.path() / "test.h";
  FileCache cache(temp_dir);
  ASSERT_TRUE(cache.Run(1));
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("const char* t = T;"_l);
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));

  // The macro of the time comes with the definition of another one.
  for (const auto macro : {"__DATE__"_l, "__TIME__"_l, "__TIMESTAMP__"_l}) {
    const CommandLine cl(Immutable("-c -DT="_l) + macro);
    const auto hash = FileCache::Hash(HandledSource("t"_l), {}, cl, version);
    cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
    cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash);
    FileCache::Entry entry;
    EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry))
        << macro;
  }
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

TEST(FileCacheTest, DirectEntry_HeadersOfBuildDirectory) {
  const base::TemporaryDir temp_dir;
  const String root = (temp_dir.path() / "root").string();
  const Path build_x = Path(root) / "out" / "x";
  const Path build_y = Path(root) / "out" / "y";
  ASSERT_TRUE(base::CreateDirectory(build_x / "gen"));
  ASSERT_TRUE(base::CreateDirectory(build_y / "gen"));
  ASSERT_TRUE(base::File::Write(build_x / "gen" / "flags.h", "#define F 1"_l));
  ASSERT_TRUE(base::File::Write(build_y / "gen" / "flags.h", "#define F 2"_l));
  ASSERT_TRUE(base::File::Write(Path(root) / "a.h", "#define A"_l));

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("int f() { return F; }"_l);
  const auto hash = FileCache::Hash(HandledSource("x"_l), {}, cl, version);
  cache.Store(hash, FileCache::Entry{"object of x"_l, "deps"_l});

  // The same command in two build directories of the root - with their own
  // generated headers.
  cache.Store(orig_code, {}, cl, version, {"../../a.h", "gen/flags.h"}, {},
              build_x, hash, root, {}, CompilationTime());
  FileCache::Entry entry;
  EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, build_x, &entry, root));
  FileCache::Entry other;
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, build_y, &other, root));
}

TEST(FileCacheTest, DirectEntry_SameContentsOtherPaths) {
  const base::TemporaryDir temp_dir;
  const Path dir_a = temp_dir.path() / "a";
  const Path dir_b = temp_dir.path() / "b";
  ASSERT_TRUE(base::CreateDirectory(dir_a));
  ASSERT_TRUE(base::CreateDirectory(dir_b));

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const UnhandledSource orig_code("int main() {}"_l);
  const auto hash_a = FileCache::Hash(HandledSource("a"_l), {}, cl, version);
  const auto hash_b = FileCache::Hash(HandledSource("b"_l), {}, cl, version);
  cache.Store(hash_a, FileCache::Entry{"object a"_l, "deps a"_l});
  cache.Store(hash_b, FileCache::Entry{"object b"_l, "deps b"_l});

  // The same header moves between the include directories: the objects and
  // the dependency files differ by its path.
  ASSERT_TRUE(base::File::Write(dir_a / "x.h", "#define X"_l));
  ASSERT_TRUE(base::File::Write(dir_b / "x.h", "#define X"_l));
  cache.Store(orig_code, {}, cl, version, {dir_a / "x.h"}, {}, temp_dir,
              hash_a, String(), {}, CompilationTime());
  cache.Store(orig_code, {}, cl, version, {dir_b / "x.h"}, {}, temp_dir,
              hash_b, String(), {}, CompilationTime());

  // Only the first header is found - with its own entry.
  ASSERT_TRUE(base::File::Delete(dir_b / "x.h"));
  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
  EXPECT_EQ("deps a"_l, entry.deps);
}

TEST(FileCacheTest, DirectEntry_ShadowedHeaderInOtherCase) {
  const base::TemporaryDir temp_dir;
  if (!IgnoresCase(temp_dir.path())) {
    GTEST_SKIP() << "The file system doesn't ignore the case";
  }
  const Path dir1 = temp_dir.path() / "dir1";
  const Path dir2 = temp_dir.path() / "dir2";
  ASSERT_TRUE(base::CreateDirectory(dir1));
  ASSERT_TRUE(base::CreateDirectory(dir2));
  const List<Path> search_dirs = {dir1, dir2};
  const auto header_path = dir2 / "a.h";

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(HandledSource("int main() { return 0; }"_l),
                                    {}, cl, version);
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

  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));
  cache.Store(orig_code, {}, cl, version, {header_path}, {}, temp_dir, hash,
              String(), search_dirs, CompilationTime());
  EXPECT_TRUE(FindDirect());

  // The compiler finds the new file in another case instead of the header.
  ASSERT_TRUE(base::File::Write(dir1 / "A.H", "#define B"_l));
  EXPECT_FALSE(FindDirect());
}

TEST(FileCacheTest, DirectEntry_HeaderInOtherCase) {
  const base::TemporaryDir temp_dir;
  if (!IgnoresCase(temp_dir.path())) {
    GTEST_SKIP() << "The file system doesn't ignore the case";
  }

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(HandledSource("int main() { return 0; }"_l),
                                    {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);
  FileCache::Entry stored_entry;
  stored_entry.object = "some object code"_l;
  stored_entry.deps = "some deps"_l;
  cache.Store(hash, stored_entry);

  // The dependency file spells the header like the "#include" - not like its
  // name.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "Header.h", "#define A"_l));
  cache.Store(orig_code, {}, cl, version, {temp_dir.path() / "header.h"}, {},
              temp_dir, hash, String(), {}, CompilationTime());
  {
    FileCache::Entry entry;
    EXPECT_TRUE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
  }

  // The modification through the real name is seen.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "Header.h", "#define B"_l));
  FileCache::Entry entry;
  EXPECT_FALSE(cache.Find(orig_code, {}, cl, version, temp_dir, &entry));
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

TEST(FileCacheTest, DirectEntry_ShadowedHeaderWithRoot) {
  const base::TemporaryDir temp_dir;
  const Path root1 = temp_dir.path() / "root1";
  const Path root2 = temp_dir.path() / "root2";
  for (const auto& root : {root1, root2}) {
    ASSERT_TRUE(base::CreateDirectory(root / "inc1"));
    ASSERT_TRUE(base::CreateDirectory(root / "inc2"));
    ASSERT_TRUE(base::File::Write(root / "inc2" / "a.h", "#define A"_l));
    ASSERT_TRUE(base::File::Write(root / "inc2" / "b.h", "#define B"_l));
    // Isn't used, e.g. because of "#include_next".
    ASSERT_TRUE(base::File::Write(root / "inc1" / "b.h", "#define C"_l));
  }
  auto SearchDirs = [](const Path& root) {
    return List<Path>{root / "inc1", root / "inc2"};
  };

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const CommandLine cl("-c -I<ROOT>/inc1 -I<ROOT>/inc2"_l);
  const Version version("1"_l);
  const UnhandledSource code("#include \"a.h\""_l);
  const auto hash = FileCache::Hash(HandledSource("1"_l), {}, cl, version);
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l, Immutable()});
  cache.Store(code, {}, cl, version, {"inc2/a.h", "inc2/b.h"}, {}, root1,
              hash, root1.string(), SearchDirs(root1), CompilationTime());

  auto FindDirect = [&](const Path& root) {
    FileCache::Entry entry;
    return cache.Find(code, {}, cl, version, root, &entry, root.string(),
                      SearchDirs(root));
  };
  // The ignored path is relative to the root: it's ignored in the other
  // checkout too.
  EXPECT_TRUE(FindDirect(root1));
  EXPECT_TRUE(FindDirect(root2));

  // A new file in the other checkout shadows the header only there.
  ASSERT_TRUE(base::File::Write(root2 / "inc1" / "a.h", "#define D"_l));
  EXPECT_FALSE(FindDirect(root2));
  EXPECT_TRUE(FindDirect(root1));
}

TEST(FileCacheTest, DirectEntry_ShadowedByIncluderDir) {
  // A quoted include is searched in the directory of the including file
  // first: e.g. "lib/a.h" includes "util.h", that is found in "include".
  const base::TemporaryDir temp_dir;
  const Path include_dir = temp_dir.path() / "include";
  const Path lib_dir = temp_dir.path() / "lib";
  ASSERT_TRUE(base::CreateDirectory(include_dir));
  ASSERT_TRUE(base::CreateDirectory(lib_dir));
  const List<Path> search_dirs = {include_dir};
  const auto includer_path = lib_dir / "a.h";
  const auto header_path = include_dir / "util.h";
  // The headers with the same names in both directories are used both.
  const auto config_path = include_dir / "config.h";
  const auto lib_config_path = lib_dir / "config.h";

  FileCache cache(temp_dir.path() / "cache");
  ASSERT_TRUE(cache.Run(1));

  const HandledSource code("int main() { return 0; }"_l);
  const CommandLine cl("-c"_l);
  const Version version("3.5 (revision 100000)"_l);
  const auto hash = FileCache::Hash(code, {}, cl, version);
  const UnhandledSource orig_code("int main() {}"_l);
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});

  auto FindDirect = [&] {
    FileCache::Entry entry;
    return cache.Find(orig_code, {}, cl, version, temp_dir, &entry, String(),
                      search_dirs);
  };

  for (const auto& path :
       {includer_path, header_path, config_path, lib_config_path}) {
    ASSERT_TRUE(base::File::Write(path, "#define A"_l));
  }
  cache.Store(orig_code, {}, cl, version,
              {includer_path, header_path, config_path, lib_config_path}, {},
              temp_dir, hash, String(), search_dirs, CompilationTime());
  EXPECT_TRUE(FindDirect());

  // A new file would be found instead of the header.
  ASSERT_TRUE(base::File::Write(lib_dir / "util.h", "#define B"_l));
  EXPECT_FALSE(FindDirect());
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
    EXPECT_EQ(std::max(base::ModificationTime(st), base::StatusChangeTime(st)),
              modified);
  }
}

TEST(FileHashCacheTest, Bounded) {
  const base::TemporaryDir temp_dir;
  FileHashCache hashes(2);
  const Path paths[] = {temp_dir.path() / "1.h", temp_dir.path() / "2.h",
                        temp_dir.path() / "3.h"};
  for (const auto& path : paths) {
    ASSERT_TRUE(base::File::Write(path, Immutable(path.string())));
  }
  // The hashes of the recently modified files aren't memoized.
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));

  String hash;
  ASSERT_TRUE(hashes.Hash(paths[0], &hash));
  ASSERT_TRUE(hashes.Hash(paths[1], &hash));
  EXPECT_EQ(2u, hashes.size());
  ASSERT_TRUE(hashes.Hash(paths[2], &hash));
  EXPECT_GE(2u, hashes.size());
  EXPECT_LT(0u, hashes.size());

  // The removed files are forgotten.
  FileHashCache other_hashes;
  ASSERT_TRUE(other_hashes.Hash(paths[0], &hash));
  ASSERT_TRUE(other_hashes.Hash(paths[1], &hash));
  ASSERT_EQ(2u, other_hashes.size());
  ASSERT_TRUE(base::File::Delete(paths[0]));
  EXPECT_FALSE(other_hashes.Hash(paths[0], &hash));
  EXPECT_EQ(1u, other_hashes.size());
}

TEST(FileHashCacheTest, Memoized) {
  const base::TemporaryDir temp_dir;
  const auto path = temp_dir.path() / "test.h";
  ASSERT_TRUE(base::File::Write(path, "first"_l));

  // The recently modified files aren't memoized.
  FileHashCache racy_hashes;
  String hash;
  ASSERT_TRUE(racy_hashes.Hash(path, &hash));
  EXPECT_EQ(0u, racy_hashes.size());

  // Any file is old enough.
  FileHashCache hashes(1'000'000, -1'000'000'000'000);
  ASSERT_TRUE(hashes.Hash(path, &hash));
  EXPECT_EQ(base::Hexify(Immutable("first"_l).Hash()), hash);
  EXPECT_EQ(1u, hashes.size());

  // The memoized hash is reused.
  hashes.entries_[path].hash = "memoized";
  hashes.entries_[path].time_macros = true;
  bool time_macros = false;
  ASSERT_TRUE(hashes.Hash(path, &hash, nullptr, &time_macros));
  EXPECT_EQ("memoized", hash);
  EXPECT_TRUE(time_macros);

  struct stat st;
  ASSERT_EQ(0, stat(path.c_str(), &st));
  const i64 mtime = base::ModificationTime(st);
  const i64 second = 1'000'000'000;
  const struct timespec times[] = {{0, UTIME_OMIT},
                                   {mtime / second, mtime % second}};
  // The times of the files come from the coarse clock: the next changes get
  // another time after its tick. The racy interval is about the same.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Modified in place with the same size and the restored mtime: the ctime
  // differs.
  {
    // Not through a temporary file - the inode stays the same.
    const int fd = open(path.c_str(), O_WRONLY);
    ASSERT_NE(-1, fd);
    EXPECT_EQ(5, write(fd, "other", 5));
    close(fd);
  }
  ASSERT_EQ(0, utimensat(AT_FDCWD, path.c_str(), times, 0));
  ASSERT_TRUE(hashes.Hash(path, &hash));
  EXPECT_EQ(base::Hexify(Immutable("other"_l).Hash()), hash);

  // Replaced by another file with the same size and mtime: the inode
  // differs.
  const auto other_path = temp_dir.path() / "other.h";
  ASSERT_TRUE(base::File::Write(other_path, "third"_l));
  ASSERT_EQ(0, utimensat(AT_FDCWD, other_path.c_str(), times, 0));
  ASSERT_TRUE(base::File::Move(other_path, path));
  ASSERT_TRUE(hashes.Hash(path, &hash));
  EXPECT_EQ(base::Hexify(Immutable("third"_l).Hash()), hash);
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

TEST(FileCacheTest, KeepHitsOnClose) {
  // The hits are applied to the index by the cleaner - in batches: the last
  // ones are applied at the close.
  const base::TemporaryDir temp_dir;
  const auto hash = TestHash("-c");
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
  ASSERT_TRUE(cache.Run(3600));
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  cache.FlushAccesses();
  cache.WaitForCleaner();
  FileCache::Entry entry;
  ASSERT_TRUE(cache.Find(hash, &entry));
  ASSERT_TRUE(cache.Find(hash, &entry));
  cache.Close();

  // The daemon may exit right after the close.
  SQLite index(temp_dir.path(), "index");
  SQLite::Value value;
  ASSERT_TRUE(index.Get(hash.str.string_copy(), &value));
  EXPECT_EQ(2u, std::get<SQLite::HITS>(value));
}

TEST(FileCacheTest, NoStoresAfterClose) {
  // The daemon exits right after the close, while the compilations still go
  // on: the stores after it wouldn't be indexed, and the files wouldn't be
  // checked at the next start - their blobs would be removed.
  const base::TemporaryDir temp_dir;
  FileCache cache(temp_dir, FileCache::UNLIMITED, proto::NONE, true);
  ASSERT_TRUE(cache.Run(1));
  cache.Close();

  const auto hash = TestHash("-c");
  cache.Store(hash, FileCache::Entry{"object"_l, "deps"_l});
  const auto manifest_path =
      AppendExtension(cache.CommonPath(hash), base::kExtManifest);
  EXPECT_FALSE(base::File::Exists(manifest_path));
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
    ASSERT_TRUE(base::SaveBinaryToFile(old_entry, manifest));
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
  // The sizes are reported by the cleaner only.
  std::promise<void> done;
  cache.cleaner_.Push([&] {
    cache.ReportSizes();
    done.set_value();
  });
  done.get_future().wait();
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
    cache.FlushAccesses();
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
