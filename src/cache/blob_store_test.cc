#include <cache/blob_store.h>

#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace dist_clang {
namespace cache {

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

  struct rlimit old_limit;
  ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &old_limit));
  struct rlimit limit = old_limit;
  limit.rlim_cur = 64;
  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &limit));
  List<int> fds;
  for (int fd; (fd = open("/dev/null", O_RDONLY)) != -1;) {
    fds.push_back(fd);
  }
  Immutable content;
  Path path;
  ui64 size = 0;
  const bool found = store.Get(hash, &content, &path, &size);
  for (int fd : fds) {
    close(fd);
  }
  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &old_limit));

  EXPECT_FALSE(found);
  EXPECT_TRUE(base::File::Exists(store.RawPath(hash)));
  EXPECT_TRUE(store.Get(hash, &content, &path, &size));
}

}  // namespace cache
}  // namespace dist_clang
