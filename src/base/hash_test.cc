#include <base/hash.h>

#include <base/const_string.h>
#include <base/string_utils.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(HashTest, MurmurHash3_x64_128) {
  ASSERT_EQ("c9e92e37df1e856cbd0abffe104225b8"_l,
            Hexify(MakeHash("All your base are belong to us"_l)));
}

}  // namespace base
}  // namespace dist_clang
