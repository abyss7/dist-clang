#include <client/invocation.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace client {

TEST(InvocationTest, Launcher) {
  auto invocation =
      ParseInvocation({"/usr/bin/dist-clang", "clang++", "-c", "a.cc"}, "");
  EXPECT_EQ(Invocation::COMPILE, invocation.action);
  EXPECT_EQ((List<String>{"clang++", "-c", "a.cc"}), invocation.args);
  EXPECT_FALSE(invocation.restat);
  EXPECT_FALSE(invocation.recache);
  EXPECT_TRUE(invocation.tags.empty());

  // The options go before the compiler.
  invocation = ParseInvocation({"dist-clang", "--restat", "--recache",
                                "--tag=main", "--tag=", "--tag=a\nb  c",
                                "--tag=gn/1", "clang++", "--restat"},
                               "");
  EXPECT_EQ(Invocation::COMPILE, invocation.action);
  EXPECT_TRUE(invocation.restat);
  EXPECT_TRUE(invocation.recache);
  // E.g. "$(git branch --show-current)" may give nothing. Any whitespace
  // separates the tags.
  EXPECT_EQ((List<String>{"main", "a", "b", "c", "gn/1"}), invocation.tags);
  EXPECT_EQ((List<String>{"clang++", "--restat"}), invocation.args);

  for (const List<String>& argv :
       {List<String>{"dist-clang"}, List<String>{"dist-clang", "--help"},
        List<String>{"dist-clang", "--restat"}}) {
    EXPECT_EQ(Invocation::USAGE, ParseInvocation(argv, "").action);
  }
}

TEST(InvocationTest, TagsVariable) {
  // The tags of the command line go after the ones of the environment.
  EXPECT_EQ("a\nb\nc", TagsVariable(" a\tb ", {"c"}));
  EXPECT_EQ("c", TagsVariable("", {"c"}));
  EXPECT_EQ("a", TagsVariable("a", {}));
}

TEST(InvocationTest, Symlink) {
  // The flags, that the real compiler takes from its name, are added.
  auto invocation = ParseInvocation(
      {"/usr/local/bin/aarch64-linux-gnu-clang++", "-c", "--restat", "a.cc"},
      "/usr/bin/clang-18");
  EXPECT_EQ(Invocation::COMPILE, invocation.action);
  EXPECT_EQ((List<String>{"/usr/bin/clang-18", "--driver-mode=g++",
                          "--target=aarch64-linux-gnu", "-c", "--restat",
                          "a.cc"}),
            invocation.args);
  EXPECT_FALSE(invocation.restat);

  invocation = ParseInvocation({"clang", "-c", "a.c"}, "");
  EXPECT_EQ(Invocation::ERROR, invocation.action);
  EXPECT_NE(String::npos, invocation.error.find("DC_CLANG_PATH"))
      << invocation.error;
}

TEST(InvocationTest, FindCompilerOnlyForSymlink) {
  // The launcher takes the compiler from the command line: it doesn't look
  // for the real one in PATH.
  ui32 lookups = 0;
  auto Lookup = [&lookups] {
    ++lookups;
    return String("/usr/bin/clang-18");
  };
  auto invocation = ParseInvocation({"dist-clang", "clang", "-c", "a.c"},
                                    Fn<String()>(Lookup));
  EXPECT_EQ(Invocation::COMPILE, invocation.action);
  EXPECT_EQ(0u, lookups);

  invocation = ParseInvocation({"clang", "-c", "a.c"}, Fn<String()>(Lookup));
  EXPECT_EQ(Invocation::COMPILE, invocation.action);
  EXPECT_EQ("/usr/bin/clang-18", invocation.args.front());
  EXPECT_EQ(1u, lookups);
}

TEST(InvocationTest, Commands) {
  auto invocation = ParseInvocation({"dist-clang", "--help-config"}, "");
  EXPECT_EQ(Invocation::HELP_CONFIG, invocation.action);
  invocation = ParseInvocation({"dist-clang", "--help-config", "x"}, "");
  EXPECT_EQ(Invocation::USAGE, invocation.action);

  invocation = ParseInvocation({"dist-clang", "--stop"}, "");
  EXPECT_EQ(Invocation::STOP, invocation.action);
  invocation = ParseInvocation({"dist-clang", "--stop", "x"}, "");
  EXPECT_EQ(Invocation::USAGE, invocation.action);

  invocation = ParseInvocation({"dist-clang", "--stats"}, "");
  EXPECT_EQ(Invocation::STATS, invocation.action);
  EXPECT_FALSE(invocation.raw);
  invocation = ParseInvocation({"dist-clang", "--stats", "--raw"}, "");
  EXPECT_EQ(Invocation::STATS, invocation.action);
  EXPECT_TRUE(invocation.raw);

  EXPECT_EQ(Invocation::TAG_STATS,
            ParseInvocation({"dist-clang", "--tag-stats"}, "").action);
  EXPECT_EQ(Invocation::FORGET_REMOVED,
            ParseInvocation({"dist-clang", "--forget-removed"}, "").action);

  invocation = ParseInvocation({"dist-clang", "--forget-tag=gn/1"}, "");
  EXPECT_EQ(Invocation::FORGET_TAG, invocation.action);
  EXPECT_EQ("gn/1", invocation.tag);
  EXPECT_FALSE(invocation.force);
  invocation =
      ParseInvocation({"dist-clang", "--forget-untagged", "--force"}, "");
  EXPECT_EQ(Invocation::FORGET_TAG, invocation.action);
  EXPECT_EQ("", invocation.tag);
  EXPECT_TRUE(invocation.force);

  // The extra arguments aren't the compiler - e.g. "--forget-removed" mustn't
  // be run.
  for (const List<String>& argv :
       {List<String>{"dist-clang", "--stats", "--raw", "x"},
        List<String>{"dist-clang", "--stats", "x"},
        List<String>{"dist-clang", "--tag-stats", "x"},
        List<String>{"dist-clang", "--forget-removed", "x"},
        List<String>{"dist-clang", "--forget-tag="},
        List<String>{"dist-clang", "--forget-tag=a", "--force", "x"},
        List<String>{"dist-clang", "--forget-untagged", "x"}}) {
    EXPECT_EQ(Invocation::USAGE, ParseInvocation(argv, "").action)
        << argv.back();
  }
}

}  // namespace client
}  // namespace dist_clang
