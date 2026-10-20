#include <cache/blob_store.h>

#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
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

}  // namespace cache
}  // namespace dist_clang
