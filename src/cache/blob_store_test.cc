#include <cache/blob_store.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include STL(thread)

namespace dist_clang {
namespace cache {

namespace {

// Runs |action| without the free descriptors: nothing can be opened.
template <class Action>
void WithoutDescriptors(Action action) {
  struct rlimit old_limit;
  ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &old_limit));
  struct rlimit limit = old_limit;
  limit.rlim_cur = 64;
  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &limit));
  List<int> fds;
  for (int fd; (fd = open("/dev/null", O_RDONLY)) != -1;) {
    fds.push_back(fd);
  }
  action();
  for (int fd : fds) {
    close(fd);
  }
  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &old_limit));
}

}  // namespace

TEST(BlobStoreTest, KeepBlobOnFailedHash) {
  // The linked blob is hashed at every hit: the failure to hash - e.g. out of
  // the descriptors - doesn't mean, that the blob is broken.
  const base::TemporaryDir temp_dir;
  const BlobStore store(temp_dir.path(), proto::NONE, true);
  const auto output = temp_dir.path() / "test.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  String hash, error;
  bool reused = false;
  ASSERT_TRUE(store.Put(Immutable(), output, &hash, &reused, &error)) << error;

  Immutable content;
  Path path;
  ui64 size = 0;
  bool found = true;
  WithoutDescriptors([&] { found = store.Get(hash, &content, &path, &size); });

  EXPECT_FALSE(found);
  EXPECT_TRUE(base::File::Exists(store.RawPath(hash)));
  EXPECT_TRUE(store.Get(hash, &content, &path, &size));
}

TEST(BlobStoreTest, IgnoreWrongHashes) {
  // The hashes come from the manifests - a broken one mustn't lead outside
  // of the store.
  const base::TemporaryDir temp_dir;
  const Path root = temp_dir.path() / "a" / "b" / "blobs";
  ASSERT_TRUE(base::CreateDirectory(root));
  BlobStore store(root, proto::ZSTD, false);
  const Path victim = temp_dir.path() / "a" / "victim";
  ASSERT_TRUE(base::File::Write(victim, "not a blob"_l));

  Immutable content;
  ui64 size = 0;
  EXPECT_FALSE(store.Get("../victim", &content, nullptr, &size));
  EXPECT_TRUE(base::File::Exists(victim));

  store.Discard("../victim");
  EXPECT_TRUE(base::File::Exists(victim));

  store.AddRef("../victim", 1);
  store.RemoveRef("../victim");
  EXPECT_TRUE(base::File::Exists(victim));
}

TEST(BlobStoreTest, VerifyLinkedBlobOnce) {
  // The linked blob may be modified in place through its links - but then its
  // times change.
  const base::TemporaryDir temp_dir;
  const BlobStore store(temp_dir.path(), proto::NONE, true);
  const auto output = temp_dir.path() / "test.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  String hash, error;
  bool reused = false;
  ASSERT_TRUE(store.Put(Immutable(), output, &hash, &reused, &error)) << error;
  // The recently modified files may still be modified within the same time.
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));

  Immutable content;
  Path path;
  ui64 size = 0;
  ASSERT_TRUE(store.Get(hash, &content, &path, &size));

  // Isn't read again.
  bool found = false;
  WithoutDescriptors([&] { found = store.Get(hash, &content, &path, &size); });
  EXPECT_TRUE(found);

  // The modified blob is read again - and is broken.
  {
    const int fd = open(store.RawPath(hash).c_str(), O_WRONLY);
    ASSERT_NE(-1, fd);
    ASSERT_EQ(6, pwrite(fd, "OBJECT", 6, 0));
    close(fd);
  }
  EXPECT_FALSE(store.Get(hash, &content, &path, &size));
  EXPECT_FALSE(base::File::Exists(store.RawPath(hash)));
}

TEST(BlobStoreTest, KeepCheckOfRestoredBlob) {
  // The restore of the blob by the daemon itself - with a hard link and a
  // touch - changes its times, but not its contents. The same for the removal
  // of the link.
  const base::TemporaryDir temp_dir;
  const BlobStore store(temp_dir.path(), proto::NONE, true);
  const auto output = temp_dir.path() / "test.o";
  const auto restored = temp_dir.path() / "restored.o";
  ASSERT_TRUE(base::File::Write(output, "object"_l));
  String hash, error;
  bool reused = false;
  ASSERT_TRUE(store.Put(Immutable(), output, &hash, &reused, &error)) << error;
  ASSERT_TRUE(base::File::Delete(output));
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));

  Immutable content;
  Path path;
  ui64 size = 0;
  ASSERT_TRUE(store.Get(hash, &content, &path, &size));
  ASSERT_TRUE(store.IsVerified(hash));
  base::File::CloneMode mode;
  ASSERT_TRUE(base::File::Clone(path, restored, true, &mode));
  ASSERT_EQ(0, utimensat(AT_FDCWD, restored.c_str(), nullptr, 0));
  EXPECT_EQ(mode != base::File::CloneMode::HARDLINK, store.IsVerified(hash));
  store.Restored(hash);
  EXPECT_TRUE(store.IsVerified(hash));
  // The build directory drops the link, e.g. at the rebuild.
  ASSERT_TRUE(base::File::Delete(restored));
  EXPECT_TRUE(store.IsVerified(hash));

  bool found = false;
  WithoutDescriptors([&] { found = store.Get(hash, &content, &path, &size); });
  EXPECT_TRUE(found);
}

}  // namespace cache
}  // namespace dist_clang
