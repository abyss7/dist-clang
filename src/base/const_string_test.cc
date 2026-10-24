#include <base/const_string.h>

#include <base/string_utils.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ConstStringTest, PrefixConstructor) {
  ConstString string("abcdef"_l);

  ConstString prefix(string, 4);
  EXPECT_EQ(4u, prefix.size());
  EXPECT_EQ("abcd"_l, prefix.string_copy(false));

  ConstString prefix_too_large(string, 10);
  EXPECT_EQ(6u, prefix_too_large.size());
  EXPECT_EQ("abcdef"_l, prefix_too_large.string_copy(false));
}

TEST(ConstStringTest, Find) {
  ConstString string("cdabcdcef"_l);
  EXPECT_EQ(4u, string.find("cdc"));
  EXPECT_EQ(String::npos, string.find("zz"));
  EXPECT_EQ(0u, string.find(""));
  EXPECT_EQ(0u, string.find("cdab"));
  EXPECT_EQ(7u, string.find("ef"));
  EXPECT_EQ(String::npos, string.find("efg"));
  EXPECT_EQ(String::npos, ConstString().find("a"));

  // The prefix shares the buffer of the whole string - the rest isn't seen.
  ConstString prefix(string, 6);
  EXPECT_EQ(3u, prefix.find("bcd"));
  EXPECT_EQ(String::npos, prefix.find("dc"));
  EXPECT_EQ(String::npos, prefix.find("ef"));

  // The match may span the parts of a rope.
  ConstString rope(ConstString::Rope{"cda"_l, "bcd"_l, "cef"_l});
  EXPECT_EQ(4u, rope.find("cdc"));
  EXPECT_EQ(1u, rope.find("dab"));
  EXPECT_EQ(String::npos, rope.find("zz"));
}

TEST(ConstStringTest, Hash) {
  {
    const auto expected_hash = "c9e92e37df1e856cbd0abffe104225b8"_l;
    EXPECT_EQ(expected_hash,
              Hexify(ConstString("All your base are belong to us"_l).Hash()));
    EXPECT_EQ(expected_hash,
              Hexify(ConstString(ConstString::Rope{"All "_l, "your base"_l,
                                                   " are belong to us"_l})
                         .Hash()));
    EXPECT_EQ(expected_hash,
              Hexify(ConstString(ConstString::Rope{"All your"_l,
                                                   " base are belong to us"_l})
                         .Hash()));
  }
  {
    const auto expected_hash = "00000000000000000000000000000000"_l;
    EXPECT_EQ(expected_hash, Hexify(ConstString().Hash()));
  }
}

TEST(ConstStringTest, EmptyString) {
  ConstString empty1;
  EXPECT_EQ(String(), empty1.string_copy(false));
  EXPECT_STREQ("", empty1.c_str());

  ConstString empty2((ConstString::Rope()));
  EXPECT_EQ(String(), empty2.string_copy(false));
  EXPECT_STREQ("", empty2.c_str());

  ConstString empty3((String()));
  EXPECT_EQ(String(), empty3.string_copy(false));
  EXPECT_STREQ("", empty3.c_str());
}

TEST(ConstStringTest, CollapseRopeTest) {
  const ConstString immutable1("i1 "_l);
  const auto concatenated = immutable1 + "l1"_l;

  const ConstString immutable2("i2 "_l);
  auto resulting = immutable2 + concatenated + " l2"_l;
  EXPECT_STREQ("i2 i1 l1 l2", resulting.c_str());
}

// TODO: write a lot of tests.

}  // namespace base
}  // namespace dist_clang
