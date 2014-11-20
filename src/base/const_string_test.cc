#include <base/const_string.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ConstStringTest, Find) {
  ConstString string("cdabcdcef"_l);
  EXPECT_EQ(4u, string.find("cdc"));
  EXPECT_EQ(String::npos, string.find("zz"));
}

// TODO: write a lot of tests.

}  // namespace base
}  // namespace dist_clang
