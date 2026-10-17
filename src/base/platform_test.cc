#include <base/platform.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

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
