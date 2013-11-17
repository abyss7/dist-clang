#include "base/string_utils.h"

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(StringUtilsTest, SplitStringByChar) {
  const std::string text = " abc d e  f";
  std::list<std::string> output;
  SplitString<' '>(text, output);

  auto it = output.begin();
  ASSERT_EQ(6u, output.size());
  EXPECT_EQ(std::string(), *it++);
  EXPECT_EQ("abc", *it++);
  EXPECT_EQ("d", *it++);
  EXPECT_EQ("e", *it++);
  EXPECT_EQ(std::string(), *it++);
  EXPECT_EQ("f", *it++);
}

TEST(StringUtilsTest, SplitStringByEOL) {
  const std::string text = "\nabc\nd\ne\n\nf";
  std::list<std::string> output;
  SplitString<'\n'>(text, output);

  auto it = output.begin();
  ASSERT_EQ(6u, output.size());
  EXPECT_EQ(std::string(), *it++);
  EXPECT_EQ("abc", *it++);
  EXPECT_EQ("d", *it++);
  EXPECT_EQ("e", *it++);
  EXPECT_EQ(std::string(), *it++);
  EXPECT_EQ("f", *it++);
}

TEST(StringUtilsTest, SplitStringByString) {
  const std::string text = " aaabc ada eaa  faa";
  std::list<std::string> output;
  SplitString(text, "aa", output);

  auto it = output.begin();
  ASSERT_EQ(4u, output.size());
  EXPECT_EQ(" ", *it++);
  EXPECT_EQ("abc ada e", *it++);
  EXPECT_EQ("  f", *it++);
  EXPECT_EQ(std::string(), *it++);
}

TEST(StringUtilsTest, JoinString) {
  std::list<std::string> tokens = {"a", "b", " ", "c"};
  std::string output = JoinString<' '>(tokens.begin(), tokens.end());
  ASSERT_EQ("a b   c", output);
}

TEST(StringUtilsTest, Hexify) {
  const std::string expected = "000102030405060708090a0b0c0d0e0f10";
  const std::string input =
      std::string("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e"
                  "\x0f\x10", 17);
  ASSERT_EQ(expected, Hexify(input));
}

TEST(StringUtilsTest, DISABLED_Replace) {
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
