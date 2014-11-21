#include <base/const_string.h>

#include <base/string_utils.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ConstStringTest, Find) {
  ConstString string("cdabcdcef"_l);
  EXPECT_EQ(4u, string.find("cdc"));
  EXPECT_EQ(String::npos, string.find("zz"));
}

TEST(ConstStringTest, Hash) {
  ASSERT_EQ("c9e92e37df1e856cbd0abffe104225b8"_l,
            Hexify(ConstString("All your base are belong to us"_l).Hash()));
}

// TODO: write a lot of tests.

}  // namespace base
}  // namespace dist_clang
