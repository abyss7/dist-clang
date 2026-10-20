#include <base/platform.h>

#include <gtest/gtest.h>

#include <sys/resource.h>

namespace dist_clang {
namespace base {

TEST(PlatformTest, RaiseOpenFilesLimit) {
  struct rlimit original;
  ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &original));
  struct rlimit limit = original;
  limit.rlim_cur = 256;
  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &limit));

  // Every running compiler takes several descriptors of the daemon.
  String error;
  EXPECT_TRUE(RaiseOpenFilesLimit(&error)) << error;
  ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &limit));
  EXPECT_GT(limit.rlim_cur, 256u);
  if (CurrentPlatform() == Platform::LINUX) {
    EXPECT_EQ(original.rlim_max, limit.rlim_cur);
  }

  ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &original));
}

TEST(PlatformTest, PathKey) {
  EXPECT_EQ(PathKey("/a/B.cc"), PathKey("/a/B.cc"));
  EXPECT_NE(PathKey("/a/b.cc"), PathKey("/a/c.cc"));
  // The default file system of Mac ignores the case.
  if (CurrentPlatform() == Platform::MACOSX) {
    EXPECT_EQ(PathKey("/A/b.cc"), PathKey("/a/B.CC"));
  } else {
    EXPECT_NE(PathKey("/A/b.cc"), PathKey("/a/B.CC"));
  }
}

}  // namespace base
}  // namespace dist_clang
