#include <base/base.pb.h>
#include <base/file_utils.h>
#include <client/command.hh>

#include <third_party/gtest/exported/include/gtest/gtest.h>
#include STL(regex)

namespace dist_clang {
namespace client {

TEST(CommandTest, MissingArgument) {
  const int argc = 4;
  const char* argv[] = {"clang++", "-c", "/tmp/some_random.cc", "-Xclang",
                        nullptr};

  Command::List commands;
  ASSERT_FALSE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_TRUE(commands.empty());
}

TEST(CommandTest, UnknownArgument) {
  const int argc = 4;
  const char* argv[] = {"clang++", "-12", "-c", "/tmp/some_random.cc", nullptr};

  Command::List commands;
  ASSERT_FALSE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_TRUE(commands.empty());
}

TEST(CommandTest, ParseSimpleArgs) {
  auto rep = [](const char* str) {
    return std::make_pair(std::regex(str), String(str));
  };

  auto rep2 = [](const String& str) {
    return std::make_pair(std::regex(str), str);
  };

  const String expected_input = "/tmp/input.cc";
  const String expected_output = "/tmp/output.o";
  List<Pair<std::regex, String>> expected_regex;
  expected_regex.push_back(rep("-cc1"));
  expected_regex.push_back(rep("-triple [a-z0-9_]+-[a-z0-9_]+-[a-z0-9]+"));
  expected_regex.push_back(rep("-emit-obj"));
  expected_regex.push_back(rep("-mrelax-all"));
  expected_regex.push_back(rep("-disable-free"));
  expected_regex.push_back(rep("-main-file-name input\\.cc"));
  expected_regex.push_back(rep("-mrelocation-model (static|pic)"));
  expected_regex.push_back(rep("-mdisable-fp-elim"));
  expected_regex.push_back(rep("-masm-verbose"));
  expected_regex.push_back(rep("-munwind-tables"));
  expected_regex.push_back(rep("-target-cpu [a-z0-9_]+"));
  expected_regex.push_back(rep("-coverage-notes-file /tmp/output.gcno"));
  expected_regex.push_back(rep("-resource-dir"));
  expected_regex.push_back(rep("-fdeprecated-macro"));
  expected_regex.push_back(rep("-fdebug-compilation-dir"));
  expected_regex.push_back(rep("-ferror-limit [0-9]+"));
  expected_regex.push_back(rep("-fmessage-length [0-9]+"));
  expected_regex.push_back(rep("-fobjc-runtime="));
  expected_regex.push_back(rep("-fcxx-exceptions"));
  expected_regex.push_back(rep("-fexceptions"));
  expected_regex.push_back(rep("-fdiagnostics-show-option"));
  expected_regex.push_back(rep2("-o " + expected_output));
  expected_regex.push_back(rep("-x c\\+\\+"));
  expected_regex.push_back(rep2(expected_input));
#if defined(OS_LINUX)
  expected_regex.push_back(rep("-fmath-errno"));
  expected_regex.push_back(rep("-mconstructor-aliases"));
  expected_regex.push_back(rep("-internal-isystem"));
  expected_regex.push_back(rep("-internal-externc-isystem"));
#elif defined(OS_MACOSX)
  expected_regex.push_back(rep("-target-linker-version [0-9.]+"));
  expected_regex.push_back(rep("-pic-level [0-9]+"));
  expected_regex.push_back(rep("-stack-protector [0-9]+"));
  expected_regex.push_back(rep("-fblocks"));
  expected_regex.push_back(rep("-fencode-extended-block-signature"));
#endif

  const char* argv[] = {
      "clang++", "-c", expected_input.c_str(), "-o", expected_output.c_str(),
      nullptr};
  const int argc = 5;

  Command::List commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(1u, commands.size());

  const auto& command = commands.front();
  for (const auto& regex : expected_regex) {
    EXPECT_TRUE(std::regex_search(command->RenderAllArgs(), regex.first))
        << "Regex \"" << regex.second << "\" failed";
  }

  if (HasNonfatalFailure()) {
    FAIL() << command->RenderAllArgs();
  }
}

TEST(CommandTest, ParseCC1Args) {
  auto rep = [](const char* str) {
    return std::make_pair(std::regex(str), String(str));
  };

  auto rep2 = [](const String& str) {
    return std::make_pair(std::regex(str), str);
  };

  const String input_name = "input.cc";
  const String expected_input = "/tmp/" + input_name;
  const String expected_output = "/tmp/output.o";
  List<Pair<std::regex, String>> expected_regex;
  expected_regex.push_back(rep("-cc1"));
  expected_regex.push_back(rep2("-main-file-name " + input_name));
  expected_regex.push_back(rep2("-coverage-file " + expected_output));
  expected_regex.push_back(rep2("-o " + expected_output));
  expected_regex.push_back(rep("-x c\\+\\+"));
  expected_regex.push_back(rep2(expected_input));

  // Even unknown args should be parsed:
  expected_regex.push_back(rep("-really_unknown_arg"));

  const char* argv[] = {"clang",
                        "-cc1",
                        "-really_unknown_arg",
                        "-main-file-name",
                        input_name.c_str(),
                        "-coverage-file",
                        expected_output.c_str(),
                        "-o",
                        expected_output.c_str(),
                        "-x",
                        "c++",
                        expected_input.c_str(),
                        nullptr};
  const int argc = 12;

  Command::List commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(1u, commands.size());

  auto& command = commands.front();
  for (const auto& regex : expected_regex) {
    EXPECT_TRUE(std::regex_search(command->RenderAllArgs(), regex.first))
        << "Regex \"" << regex.second << "\" failed";
  }

  base::proto::Flags flags;
  ASSERT_TRUE(command->CanFillFlags());
  ASSERT_TRUE(command->FillFlags(&flags, "/some/clang/path", "1.0.0"));

  if (HasNonfatalFailure()) {
    FAIL() << command->RenderAllArgs();
  }

  // Unknown arguments should go to |other| flags.
  bool found = false;
  for (const auto& flag : flags.other()) {
    if (flag == "-really_unknown_arg") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found) << "Unknown argument is not in |other| flags!";
}

TEST(CommandTest, FillFlags) {
  const String input = "/test_file.cc";
  const String output = "/tmp/output.o";
  const String plugin_name = "plugin";
  const char* argv[] = {
      "clang++",           "-Xclang", "-load",       "-Xclang",
      "/tmp/plugin/path",  "-Xclang", "-add-plugin", "-Xclang",
      plugin_name.c_str(), "-c",      input.c_str(), "-o",
      output.c_str(),      nullptr};
  const int argc = 13;

  Command::List commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(1u, commands.size());

  auto& command = commands.front();
  base::proto::Flags flags;
  ASSERT_TRUE(command->CanFillFlags());
  ASSERT_TRUE(command->FillFlags(&flags, "/some/clang/path", "1.0.0"));

  EXPECT_EQ(input, flags.input());
  EXPECT_EQ(output, flags.output());
  EXPECT_EQ("-emit-obj", flags.action());
  EXPECT_EQ("c++", flags.language());
  EXPECT_EQ("-cc1", *flags.other().begin());
  EXPECT_EQ(1, flags.compiler().plugins_size());
  EXPECT_EQ(plugin_name, flags.compiler().plugins(0).name());
  // TODO: add more expectations on flags, especially about version replacement.

  if (HasNonfatalFailure()) {
    FAIL() << command->RenderAllArgs();
  }
}

TEST(CommandTest, AppendCleanTempFilesCommand) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", temp_input.c_str(), nullptr};
  const int argc = 2;

  Command::List commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(3u, commands.size());
  auto& command = commands.back();

  // Can't use |CleanCommand::rm_path| since it's not a global symbol.
  EXPECT_EQ("/bin/rm", command->GetExecutable());

  if (HasNonfatalFailure()) {
    FAIL() << command->RenderAllArgs();
  }
}

}  // namespace client
}  // namespace dist_clang
