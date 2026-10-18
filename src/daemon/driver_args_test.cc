#include <daemon/driver_args.h>

#include <base/base.pb.h>
#include <base/const_string.h>
#include <base/file/file.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

namespace dist_clang {
namespace daemon {

namespace {

List<String> Values(
    const google::protobuf::RepeatedPtrField<base::proto::Argument>& args) {
  List<String> result;
  for (const auto& arg : args) {
    for (const auto& value : arg.values()) {
      result.push_back(value);
    }
  }
  return result;
}

List<String> KeyExtra(const base::proto::Flags& flags) {
  return List<String>(flags.key_extra().begin(), flags.key_extra().end());
}

bool Parse(const List<String>& args, base::proto::Flags* flags,
           String* reason = nullptr, const String& current_dir = "/tmp") {
  return ParseDriverArgs(args, current_dir, flags, reason);
}

}  // namespace

TEST(DriverArgsTest, SimpleCompilation) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-o", "test.o", "-O2"}, &flags));

  EXPECT_EQ("test.cc", flags.input());
  EXPECT_EQ("test.o", flags.output());
  EXPECT_EQ("-c", flags.action());
  EXPECT_FALSE(flags.has_language());
  EXPECT_FALSE(flags.has_deps_file());
  EXPECT_FALSE(flags.has_dwo_file());
  EXPECT_EQ(List<String>{"-O2"}, Values(flags.other()));
  EXPECT_TRUE(KeyExtra(flags).empty());
}

TEST(DriverArgsTest, DefaultOutput) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "dir/test.cpp"}, &flags));
  EXPECT_EQ("test.o", flags.output());
}

TEST(DriverArgsTest, JoinedOutput) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-otest.o", "-objcmt-migrate-literals"},
                    &flags));
  EXPECT_EQ("test.o", flags.output());
  EXPECT_EQ(List<String>{"-objcmt-migrate-literals"}, Values(flags.other()));
}

TEST(DriverArgsTest, PreprocessorFlags) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-I", "include", "-Iinclude2", "-DFOO",
                     "-D", "BAR=1", "-isystem", "sys", "-include", "pre.h",
                     "-nostdinc++", "-fmacro-prefix-map=../=", "-O2"},
                    &flags));

  const List<String> expected = {"-I",  "include", "-Iinclude2",
                                 "-DFOO", "-D",      "BAR=1",
                                 "-isystem", "sys", "-include",
                                 "pre.h", "-nostdinc++"};
  EXPECT_EQ(expected, Values(flags.non_cached()));
  // The map changes "__builtin_FILE()" in the compilation too - it's a part of
  // the key of the simple cache.
  EXPECT_EQ((List<String>{"-fmacro-prefix-map=../=", "-O2"}),
            Values(flags.other()));
}

TEST(DriverArgsTest, ArgumentsKeepOrder) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-fno-rtti", "-I", "include", "-c", "-O2", "test.cc"},
                    &flags));

  ASSERT_EQ(2, flags.other_size());
  ASSERT_EQ(1, flags.non_cached_size());
  EXPECT_LT(flags.other(0).index(), flags.non_cached(0).index());
  EXPECT_LT(flags.non_cached(0).index(), flags.other(1).index());
}

TEST(DriverArgsTest, DependencyFile) {
  base::proto::Flags flags;
  ASSERT_TRUE(
      Parse({"-c", "test.cc", "-o", "obj/test.o", "-MD", "-MF", "test.d"},
            &flags));
  EXPECT_EQ("test.d", flags.deps_file());
  EXPECT_EQ("obj/test.o", flags.deps_target());
  EXPECT_EQ((List<String>{"-MD", "-MF", "test.d"}), Values(flags.non_direct()));
  EXPECT_TRUE(Values(flags.other()).empty());
}

TEST(DriverArgsTest, DefaultDependencyFile) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-o", "obj/test.cc.o", "-MMD"}, &flags));
  EXPECT_EQ("obj/test.cc.d", flags.deps_file());
}

TEST(DriverArgsTest, SystemHeadersInDependencyFile) {
  // The direct cache must see the changes of the system headers too.
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-MMD", "-MF", "test.d"}, &flags));
  EXPECT_EQ((List<String>{"-MD", "-MF", "test.d"}), Values(flags.non_direct()));
}

TEST(DriverArgsTest, DependencyFileWithoutGeneration) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-MF", "test.d"}, &flags));
  EXPECT_FALSE(flags.has_deps_file());
}

TEST(DriverArgsTest, DependencyTargets) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-MD", "-MT", "a b", "-MQ", "c d$"},
                    &flags));
  EXPECT_EQ("a b c\\ d$$", flags.deps_target());
}

TEST(DriverArgsTest, QuoteMakeTarget) {
  EXPECT_EQ("a\\ b", QuoteMakeTarget("a b"));
  EXPECT_EQ("a\\\\\\ b", QuoteMakeTarget("a\\ b"));
  EXPECT_EQ("$$x\\#", QuoteMakeTarget("$x#"));
}

TEST(DriverArgsTest, TimeTraceIsIgnored) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-ftime-trace",
                     "-ftime-trace-granularity=500", "-ftime-trace=out.json"},
                    &flags));
  EXPECT_EQ((List<String>{"-ftime-trace", "-ftime-trace-granularity=500",
                          "-ftime-trace=out.json"}),
            Values(flags.ignored()));
  EXPECT_TRUE(Values(flags.other()).empty());
}

TEST(DriverArgsTest, DebugInfoDependsOnCurrentDir) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-g"}, &flags));
  EXPECT_EQ(List<String>{"cwd:/tmp"}, KeyExtra(flags));

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-g", "-g0"}, &flags));
  EXPECT_TRUE(KeyExtra(flags).empty());

  flags.Clear();
  ASSERT_TRUE(
      Parse({"-c", "test.cc", "-g", "-fdebug-compilation-dir=."}, &flags));
  EXPECT_TRUE(KeyExtra(flags).empty());
}

TEST(DriverArgsTest, SplitDwarf) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-o", "obj/test.o", "-g",
                     "-gsplit-dwarf"},
                    &flags));
  EXPECT_EQ("obj/test.dwo", flags.dwo_file());
  EXPECT_EQ((List<String>{"cwd:/tmp", "dwo:obj/test.dwo"}), KeyExtra(flags));

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-g", "-gsplit-dwarf=single"}, &flags));
  EXPECT_FALSE(flags.has_dwo_file());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-gsplit-dwarf"}, &flags));
  EXPECT_FALSE(flags.has_dwo_file());
}

TEST(DriverArgsTest, RecordsCommandLine) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc"}, &flags));
  EXPECT_FALSE(flags.records_command_line());

  // The default depends on the build of the compiler.
  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-g"}, &flags));
  EXPECT_TRUE(flags.records_command_line());

  flags.Clear();
  ASSERT_TRUE(
      Parse({"-c", "test.cc", "-g", "-gno-record-command-line"}, &flags));
  EXPECT_FALSE(flags.records_command_line());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-gno-record-gcc-switches", "-g",
                     "-grecord-command-line"},
                    &flags));
  EXPECT_TRUE(flags.records_command_line());

  // Doesn't enable the debug info.
  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-grecord-command-line"}, &flags));
  EXPECT_FALSE(flags.records_command_line());
  EXPECT_TRUE(KeyExtra(flags).empty());
}

TEST(DriverArgsTest, DiagnosticsFormat) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-fcolor-diagnostics",
                     "-fmessage-length=120", "-fdiagnostics-show-option",
                     "-fno-caret-diagnostics", "-ferror-limit=10"},
                    &flags));
  EXPECT_TRUE(Values(flags.other()).empty());
  EXPECT_EQ(5u, Values(flags.non_direct()).size());
  EXPECT_TRUE(flags.color_diagnostics());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-fdiagnostics-color=always",
                     "-fdiagnostics-color=never"},
                    &flags));
  EXPECT_FALSE(flags.color_diagnostics());
}

TEST(DriverArgsTest, Warnings) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-Wall", "-Werror", "-Wno-unused",
                     "-Werror=format", "-w", "-pedantic", "-Wl,--as-needed",
                     "-Wa,--noexecstack"},
                    &flags));
  EXPECT_EQ((List<String>{"-Wall", "-Werror", "-Wno-unused", "-Werror=format",
                          "-w", "-pedantic"}),
            Values(flags.warnings()));
  EXPECT_EQ((List<String>{"-Wl,--as-needed", "-Wa,--noexecstack"}),
            Values(flags.other()));
}

TEST(DriverArgsTest, Relocatable) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc"}, &flags));
  EXPECT_FALSE(flags.relocatable());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-fmacro-prefix-map=/src="}, &flags));
  EXPECT_TRUE(flags.relocatable());

  // The debug info has paths too.
  flags.Clear();
  ASSERT_TRUE(
      Parse({"-c", "test.cc", "-g", "-fmacro-prefix-map=/src="}, &flags));
  EXPECT_FALSE(flags.relocatable());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-g", "-ffile-prefix-map=/src=.",
                     "-fdebug-compilation-dir=."},
                    &flags));
  EXPECT_TRUE(flags.relocatable());
}

TEST(DriverArgsTest, RemappedPrefixes) {
  auto Prefixes = [](const List<String>& args) {
    base::proto::Flags flags;
    EXPECT_TRUE(Parse(args, &flags));
    return List<String>(flags.remapped_prefixes().begin(),
                        flags.remapped_prefixes().end());
  };

  EXPECT_EQ((List<String>{"../.."}),
            Prefixes({"-c", "test.cc", "-fmacro-prefix-map=../../="}));
  EXPECT_EQ((List<String>{"/src", "/other"}),
            Prefixes({"-c", "test.cc", "-fmacro-prefix-map=/src/=",
                      "-ffile-prefix-map=/other=x"}));
  // The debug info needs the maps for it too.
  EXPECT_EQ((List<String>{"/src"}),
            Prefixes({"-c", "test.cc", "-g", "-fmacro-prefix-map=/a=",
                      "-fdebug-prefix-map=/b=", "-ffile-prefix-map=/src=.",
                      "-fdebug-compilation-dir=."}));
}

TEST(DriverArgsTest, Language) {
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-x", "c++", "-c", "test.inc"}, &flags));
  EXPECT_EQ("c++", flags.language());
  EXPECT_TRUE(Values(flags.other()).empty());

  String reason;
  flags.Clear();
  EXPECT_FALSE(Parse({"-x", "c++-header", "-c", "test.h"}, &flags, &reason));
  EXPECT_FALSE(reason.empty());

  flags.Clear();
  EXPECT_FALSE(Parse({"-c", "test.cc", "-x", "c"}, &flags));

  flags.Clear();
  EXPECT_FALSE(Parse({"-c", "test.s"}, &flags));
}

TEST(DriverArgsTest, Unsupported) {
  const List<List<String>> commands = {
      {"test.cc", "-o", "test"},
      {"-c", "a.cc", "b.cc"},
      {"-c", "-"},
      {"-E", "test.cc"},
      {"-S", "test.cc"},
      {"-c", "test.cc", "-o", "-"},
      {"-c", "test.cc", "-save-temps"},
      {"-c", "test.cc", "-fmodules"},
      {"-c", "test.cc", "--coverage"},
      {"-c", "test.cc", "--config", "my.cfg"},
      {"-c", "test.cc", "-fprofile-use=missing.profdata"},
      {"-c", "test.cc", "-o"},
      // An unknown flag with a separate value looks like the second input.
      {"-c", "test.cc", "-unknown-flag", "value"},
  };

  for (const auto& command : commands) {
    base::proto::Flags flags;
    String reason;
    EXPECT_FALSE(Parse(command, &flags, &reason))
        << "Command: " << base::proto::Flags().DebugString() << command.size();
    EXPECT_FALSE(reason.empty());
  }
}

TEST(DriverArgsTest, PrecompiledHeader) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "pch.h.gch", "pch"_l));

  // The source of the implicit precompiled header is visible in the
  // preprocessed source - no need to track the PCH itself.
  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-include", "pch.h", "-include", "a.h"},
                    &flags, nullptr, temp_dir.path()));
  EXPECT_EQ(0, flags.included_files_size());
  EXPECT_EQ(4u, Values(flags.non_cached()).size());

  flags.Clear();
  ASSERT_TRUE(Parse({"-c", "test.cc", "-Xclang", "-include-pch", "-Xclang",
                     "x.pch", "-include-pch", "y.pch"},
                    &flags));
  ASSERT_EQ(2, flags.included_files_size());
  EXPECT_EQ("x.pch", flags.included_files(0));
  EXPECT_EQ("y.pch", flags.included_files(1));
}

TEST(DriverArgsTest, InputFiles) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "list.txt", "fun:*"_l));

  base::proto::Flags flags;
  ASSERT_TRUE(Parse({"-c", "test.cc", "-fsanitize-ignorelist=list.txt"},
                    &flags, nullptr, temp_dir.path()));
  ASSERT_EQ(1, flags.key_files_size());
  EXPECT_EQ("list.txt", flags.key_files(0));
  EXPECT_EQ(List<String>{"-fsanitize-ignorelist=list.txt"},
            Values(flags.other()));
}

TEST(DriverArgsTest, Environment) {
  base::proto::Flags flags;
  ASSERT_TRUE(ParseEnvironment(
      {"PATH=/bin", "SOURCE_DATE_EPOCH=1", "CPATH=/inc", "HOME=/home"}, &flags,
      nullptr));
  EXPECT_EQ(4, flags.env_size());
  EXPECT_EQ((List<String>{"env:CPATH=/inc", "env:SOURCE_DATE_EPOCH=1"}),
            KeyExtra(flags));

  String reason;
  EXPECT_FALSE(
      ParseEnvironment({"CCC_OVERRIDE_OPTIONS=+-O0"}, &flags, &reason));
  EXPECT_FALSE(reason.empty());
}

TEST(DriverArgsTest, ResponseFiles) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "args.rsp",
                                "-c 'a b.cc' \"-DX=\\\"y\\\"\" @nested.rsp"_l));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "nested.rsp", "-O2\n"_l));

  List<String> args = {"-Wall", "@args.rsp", "-g"};
  String error;
  ASSERT_TRUE(ExpandResponseFiles(args, temp_dir.path(), &error)) << error;
  EXPECT_EQ((List<String>{"-Wall", "-c", "a b.cc", "-DX=\"y\"", "-O2", "-g"}),
            args);

  args = {"@missing.rsp"};
  EXPECT_FALSE(ExpandResponseFiles(args, temp_dir.path(), &error));
}

}  // namespace daemon
}  // namespace dist_clang
