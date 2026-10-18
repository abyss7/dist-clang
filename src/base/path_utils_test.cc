#include <base/path_utils.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(PathUtilsTest, NormalPath) {
  // Like |Path::lexically_normal()|, but without the costly paths.
  EXPECT_EQ("/a/b/c.h", NormalPath("/a/b", "c.h"));
  EXPECT_EQ("/a/c.h", NormalPath("/a/b", "../c.h"));
  EXPECT_EQ("/x/c.h", NormalPath("/a/b", "/x/./y/../c.h"));
  EXPECT_EQ("/a/b/c.h", NormalPath("/a/b/", "./c.h"));
  EXPECT_EQ("/c.h", NormalPath("/a", "../../../c.h"));
  EXPECT_EQ("/a/b", NormalPath("/a//b/", "."));
  EXPECT_EQ("/", NormalPath("/a", ".."));
  for (const auto& [dir, path] : List<Pair<String>>{
           {"/build/out", "../../src/x/../y/z.h"},
           {"/build", "gen//a.h"},
           {"/", "a/b/../../c"}}) {
    EXPECT_EQ((Path(dir) / path).lexically_normal().string(),
              NormalPath(dir, path))
        << dir << " " << path;
  }
}

}  // namespace base
}  // namespace dist_clang
