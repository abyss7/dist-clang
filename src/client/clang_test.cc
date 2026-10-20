#include <client/clang.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace client {

TEST(ClangTest, TrustOnlyOwnOrRootDaemon) {
  EXPECT_TRUE(IsTrustedDaemon(1000, 1000));
  EXPECT_TRUE(IsTrustedDaemon(1000, 0));
  EXPECT_FALSE(IsTrustedDaemon(1000, 1001));
  EXPECT_TRUE(IsTrustedDaemon(0, 0));
  EXPECT_FALSE(IsTrustedDaemon(0, 1000));
}

}  // namespace client
}  // namespace dist_clang
