#include <base/string_utils.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(StringUtilsTest, SplitStringByChar) {
  const String text = " abc d e  f";
  List<String> output;
  SplitString<' '>(text, output);

  auto it = output.begin();
  ASSERT_EQ(4u, output.size());
  EXPECT_EQ("abc", *it++);
  EXPECT_EQ("d", *it++);
  EXPECT_EQ("e", *it++);
  EXPECT_EQ("f", *it++);
}

TEST(StringUtilsTest, SplitStringByEOL) {
  const String text = "\nabc\nd\ne\n\nf";
  List<String> output;
  SplitString<'\n'>(text, output);

  auto it = output.begin();
  ASSERT_EQ(4u, output.size());
  EXPECT_EQ("abc", *it++);
  EXPECT_EQ("d", *it++);
  EXPECT_EQ("e", *it++);
  EXPECT_EQ("f", *it++);
}

TEST(StringUtilsTest, SplitStringByString) {
  const String text = " aaabc ada eaa  faa";
  List<String> output;
  SplitString(text, "aa", output);

  auto it = output.begin();
  ASSERT_EQ(3u, output.size());
  EXPECT_EQ(" ", *it++);
  EXPECT_EQ("abc ada e", *it++);
  EXPECT_EQ("  f", *it++);
}

TEST(StringUtilsTest, JoinString) {
  List<String> tokens = {"a", "b", " ", "c"};
  String output = JoinString<' '>(tokens.begin(), tokens.end());
  ASSERT_EQ("a b   c", output);
}

TEST(StringUtilsTest, Hexify) {
  const String expected = "000102030405060708090a0b0c0d0e0f10";
  const String input = String(
      "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10",
      17);
  ASSERT_EQ(expected, Hexify(input));
}

TEST(StringUtilsTest, StringTo) {
  const String to_uint = "123456";
  const String to_int = "-123456";
  const String bad_to_int = "abc";

  EXPECT_EQ(123456u, StringTo<ui32>(to_uint));
  EXPECT_EQ(-123456, StringTo<i32>(to_int));
  EXPECT_EQ(0u, StringTo<ui32>(to_int));
  EXPECT_EQ(0u, StringTo<ui32>(bad_to_int));
}

TEST(StringUtilsTest, DISABLED_Replace) {
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
