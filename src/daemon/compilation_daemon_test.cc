#include <daemon/compilation_daemon.h>

#include <daemon/compiler_identity.h>
#include <daemon/driver_args.h>

#include <base/file/file.h>
#include <base/process.h>
#include <base/process_impl.h>
#include <base/temporary_dir.h>

#include <gtest/gtest.h>

#include STL(thread)

namespace dist_clang {
namespace daemon {

TEST(CompilationDaemonTest, CreateProcessFromFlags) {
  const List<Literal> expected_args = {
      "-cc1"_l,
      "-I."_l,
      "-mrelax-all"_l,
      "-emit-obj"_l,
      "-Xclang"_l,
      "-load"_l,
      "-Xclang"_l,
      "/usr/lib/libplugin.so"_l,
      "-x"_l,
      "c++"_l,
      "-fsanitize-blacklist=asan-blacklist.txt"_l,
      "-o"_l,
      "test.o"_l,
      "test.cc"_l,
  };
  const ui32 expected_user_id = 1u;

  base::proto::Flags flags;
  flags.mutable_compiler()->set_path("/usr/bin/clang");
  flags.mutable_compiler()->add_plugins()->set_path("/usr/lib/libplugin.so");
  flags.add_cc_only()->add_values("-mrelax-all");
  flags.add_non_cached()->add_values("-I.");
  flags.add_other()->add_values("-cc1");
  flags.set_action("-emit-obj");
  flags.set_input("test.cc");
  flags.set_output("test.o");
  flags.set_language("c++");
  flags.set_deps_file("some_deps_file");
  flags.set_sanitize_blacklist("asan-blacklist.txt");

  {
    base::ProcessPtr process =
        CompilationDaemon::CreateProcess(flags, expected_user_id);
    auto it = expected_args.begin();
    ASSERT_EQ(expected_args.size(), process->args_.size());
    for (const auto& arg : process->args_) {
      EXPECT_EQ(*(it++), arg);
    }
    EXPECT_EQ(Path("/usr/bin/clang"), process->exec_path_);
    EXPECT_EQ(expected_user_id, process->uid_);
  }

  {
    base::ProcessPtr process = CompilationDaemon::CreateProcess(flags);
    auto it = expected_args.begin();
    ASSERT_EQ(expected_args.size(), process->args_.size());
    for (const auto& arg : process->args_) {
      EXPECT_EQ(*(it++), arg);
    }
    EXPECT_EQ(Path("/usr/bin/clang"), process->exec_path_);
    EXPECT_EQ(base::Process::SAME_UID, process->uid_);
  }
}

TEST(CompilationDaemonTest, IncludeSearchDirs) {
  base::proto::Flags flags;
  String reason;
  ASSERT_TRUE(ParseDriverArgs(
      {"-idirafter", "after", "-isystem/sys", "-I", "../src/",
       "-iquote", "quote", "-Igen", "-c", "dir/test.cc", "-o", "test.o",
       "-I../src", "-isystem-after", "after2", "-D", "A", "-Iinc/./x/.."},
      "/build", &flags, &reason))
      << reason;

  EXPECT_EQ((List<Path>{"/build/dir", "/build/quote", "/src", "/build/gen",
                        "/build/inc", "/sys", "/build/after",
                        "/build/after2"}),
            CompilationDaemon::IncludeSearchDirs(flags, "/build"));

  // The implicit directories of the compiler go after the system ones.
  flags.add_implicit_include_dirs("/usr/lib/../include/c++/10");
  flags.add_implicit_include_dirs("/usr/include");
  EXPECT_EQ((List<Path>{"/build/dir", "/build/quote", "/src", "/build/gen",
                        "/build/inc", "/sys", "/usr/include/c++/10",
                        "/usr/include", "/build/after", "/build/after2"}),
            CompilationDaemon::IncludeSearchDirs(flags, "/build"));
}

TEST(CompilationDaemonTest, FindSpecialFile) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.cc", "int x;"_l));
  base::proto::Flags flags;
  flags.set_input("a.cc");
  flags.set_output("a.o");
  String path;
  // The outputs, that don't exist yet, are fine.
  EXPECT_FALSE(CompilationDaemon::FindSpecialFile(flags, temp_dir, &path));

  flags.set_output("/dev/null");
  EXPECT_TRUE(CompilationDaemon::FindSpecialFile(flags, temp_dir, &path));
  EXPECT_EQ("/dev/null", path);

  flags.set_output("a.o");
  flags.set_deps_file("/dev/null");
  EXPECT_TRUE(CompilationDaemon::FindSpecialFile(flags, temp_dir, &path));

  flags.clear_deps_file();
  flags.set_input("/dev/stdin");
  EXPECT_TRUE(CompilationDaemon::FindSpecialFile(flags, temp_dir, &path));

  // A directory isn't a regular file either.
  flags.set_input("a.cc");
  flags.set_output(".");
  EXPECT_TRUE(CompilationDaemon::FindSpecialFile(flags, temp_dir, &path));
}

TEST(CompilationDaemonTest, CompilerIdentityOfDriverMode) {
  // The other tests may leave the fake processes.
  base::Process::SetFactory<base::Process::DefaultFactory>();
  const base::TemporaryDir temp_dir;
  // The fake driver takes the language of ".c" sources from its name, like
  // clang does, and shows its current directory.
  const auto driver = temp_dir.path() / "driver";
  ASSERT_TRUE(base::File::Write(driver,
                                "#!/bin/sh\n"
                                "lang=\n"
                                "while [ $# -gt 0 ]; do\n"
                                "  case \"$1\" in\n"
                                "    -x) lang=$2; shift;;\n"
                                "    *.c) case \"$0\" in\n"
                                "           *++) lang=${lang:-c++};;\n"
                                "           *) lang=${lang:-c};;\n"
                                "         esac;;\n"
                                "  esac\n"
                                "  shift\n"
                                "done\n"
                                "echo \"-cc1 -fdebug-compilation-dir=$(pwd) "
                                "-x $lang\" >&2\n"_l));
  std::filesystem::permissions(driver, std::filesystem::perms::owner_all);
  const auto c_driver = temp_dir.path() / "clang";
  const auto cxx_driver = temp_dir.path() / "clang++";
  std::filesystem::create_symlink(driver, c_driver);
  std::filesystem::create_symlink(driver, cxx_driver);

  CompilerIdentity identities;
  String c_identity, cxx_identity, error;
  ASSERT_TRUE(identities.Get(c_driver, &c_identity, &error)) << error;
  ASSERT_TRUE(identities.Get(cxx_driver, &cxx_identity, &error)) << error;
  EXPECT_NE(c_identity, cxx_identity);

  // The identity is the same after the restart of the daemon.
  CompilerIdentity other_identities;
  String identity;
  ASSERT_TRUE(other_identities.Get(c_driver, &identity, &error)) << error;
  EXPECT_EQ(c_identity, identity);
}

TEST(CompilationDaemonTest, CompilerIdentityIsRechecked) {
  // The other tests may leave the fake processes.
  base::Process::SetFactory<base::Process::DefaultFactory>();
  const base::TemporaryDir temp_dir;
  // The defaults of the fake driver depend on another file - like the include
  // directories of clang depend on the installed GCC.
  const auto driver = temp_dir.path() / "clang";
  const auto toolchain = temp_dir.path() / "toolchain";
  ASSERT_TRUE(base::File::Write(
      driver, "#!/bin/sh\ncat \"$(dirname \"$0\")/toolchain\" >&2\n"_l));
  std::filesystem::permissions(driver, std::filesystem::perms::owner_all);
  ASSERT_TRUE(base::File::Write(toolchain, "gcc-13"_l));

  CompilerIdentity identities(std::chrono::milliseconds(200));
  String old_identity, identity, error;
  ASSERT_TRUE(identities.Get(driver, &old_identity, &error)) << error;

  // The identity is remembered for a while.
  ASSERT_TRUE(base::File::Write(toolchain, "gcc-14"_l));
  ASSERT_TRUE(identities.Get(driver, &identity, &error)) << error;
  EXPECT_EQ(old_identity, identity);

  // And then is taken again.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ASSERT_TRUE(identities.Get(driver, &identity, &error)) << error;
  EXPECT_NE(old_identity, identity);
}

TEST(CompilationDaemonTest, CompilerGetsOnlyEnvironmentOfClient) {
  // The other tests may leave the fake processes.
  base::Process::SetFactory<base::Process::DefaultFactory>();
  setenv("DC_TEST_DAEMON_VARIABLE", "1", 1);

  // The fake compiler shows its environment.
  base::proto::Flags flags;
  flags.mutable_compiler()->set_path("/usr/bin/env");
  flags.set_action("-0");

  // The environment of the daemon isn't in the key: the compiler doesn't get
  // it, even if the client has no variables at all.
  auto process = CompilationDaemon::CreateProcess(flags, Path("/"));
  String error;
  ASSERT_TRUE(process->Run(10, &error)) << error;
  EXPECT_EQ(""_l, process->stdout());

  flags.add_env("A=1");
  process = CompilationDaemon::CreateProcess(flags, Path("/"));
  ASSERT_TRUE(process->Run(10, &error)) << error;
  EXPECT_EQ(Immutable(String("A=1\0", 4)), process->stdout());
  unsetenv("DC_TEST_DAEMON_VARIABLE");
}

TEST(CompilationDaemonTest, ParseImplicitIncludeDirs) {
  const String output =
      "clang version 18.1.8\n"
      "#include \"...\" search starts here:\n"
      "#include <...> search starts here:\n"
      " /usr/bin/../include/c++/10\n"
      " /usr/include\n"
      " /Library/Frameworks (framework directory)\n"
      "End of search list.\n";
  EXPECT_EQ((List<String>{"/usr/bin/../include/c++/10", "/usr/include",
                          "/Library/Frameworks"}),
            CompilerIdentity::ParseIncludeDirs(output));
  EXPECT_TRUE(CompilerIdentity::ParseIncludeDirs("error").empty());
}

TEST(CompilationDaemonTest, StripColors) {
  EXPECT_EQ("a.cc:1:1: warning: text [-Wfoo]",
            CompilationDaemon::StripColors(
                "\x1b[1ma.cc:1:1: \x1b[0;1;35mwarning: \x1b[0mtext "
                "[-Wfoo]\x1b[0m"));
  EXPECT_EQ("plain", CompilationDaemon::StripColors("plain"));
}

TEST(CompilationDaemonTest, WarningsHash) {
  base::proto::Flags flags1, flags2;
  flags1.add_warnings()->add_values("-Wall");
  flags2.add_warnings()->add_values("-Wall");
  EXPECT_EQ(CompilationDaemon::WarningsHash(flags1),
            CompilationDaemon::WarningsHash(flags2));

  flags2.add_warnings()->add_values("-Werror");
  EXPECT_NE(CompilationDaemon::WarningsHash(flags1),
            CompilationDaemon::WarningsHash(flags2));

  // The format of the diagnostics too - but not the other flags.
  base::proto::Flags flags3 = flags1;
  flags3.add_non_direct()->add_values("-MD");
  EXPECT_EQ(CompilationDaemon::WarningsHash(flags1),
            CompilationDaemon::WarningsHash(flags3));
  flags3.add_non_direct()->add_values("-fdiagnostics-format=msvc");
  EXPECT_NE(CompilationDaemon::WarningsHash(flags1),
            CompilationDaemon::WarningsHash(flags3));
}

TEST(CompilationDaemonTest, NormalizePaths) {
  base::proto::Flags flags;
  EXPECT_EQ("-I/src/a", CompilationDaemon::NormalizePaths("-I/src/a", flags));

  flags.set_key_root("/home/user/src");
  flags.set_key_root_relative("../src");
  flags.set_key_root_remapped(true);
  flags.set_key_root_relative_remapped(true);
  EXPECT_EQ(
      "-I<ROOT>/a -I<ROOT> -I<ROOT>/b -I../src.old/c \"<ROOT>\" "
      "-isystem <ROOT>/d /other/home/user/src/e x/../src/f "
      "-fmacro-prefix-map=<ROOT>/=",
      CompilationDaemon::NormalizePaths(
          "-I/home/user/src/a -I/home/user/src -I../src/b -I../src.old/c "
          "\"../src\" -isystem /home/user/src/d /other/home/user/src/e "
          "x/../src/f -fmacro-prefix-map=../src/=",
          flags));

  // The form of the root, that the compiler doesn't remap, stays.
  flags.set_key_root_remapped(false);
  EXPECT_EQ("-I/home/user/src/a -I<ROOT>/b",
            CompilationDaemon::NormalizePaths(
                "-I/home/user/src/a -I../src/b", flags));
}

TEST(CompilationDaemonTest, NormalizeLineMarkers) {
  base::proto::Flags flags;
  flags.set_key_root("/root1");
  flags.set_key_root_relative("../root1");
  flags.set_key_root_remapped(true);
  flags.set_key_root_relative_remapped(true);

  EXPECT_EQ(
      "# 1 \"<ROOT>/a.cc\"\nconst char* f = \"/root1/a.cc\";\n"
      "# 2 \"<ROOT>/b.h\" 1\n",
      CompilationDaemon::NormalizeLineMarkers(
          "# 1 \"/root1/a.cc\"\nconst char* f = \"/root1/a.cc\";\n"
          "# 2 \"../root1/b.h\" 1\n",
          flags));
}

TEST(CompilationDaemonTest, HashDoesntDependOnSourceRoot) {
  using cache::string::HandledSource;

  base::proto::Flags flags1, flags2;
  for (auto* flags : {&flags1, &flags2}) {
    flags->mutable_compiler()->set_version("1");
    flags->set_action("-c");
  }
  flags1.add_other()->add_values("-fmacro-prefix-map=../../root1/=");
  flags1.set_key_root("/home/root1");
  flags1.set_key_root_relative("../../root1");
  flags1.set_key_root_relative_remapped(true);
  flags2.add_other()->add_values("-fmacro-prefix-map=../../root2/=");
  flags2.set_key_root("/home/root2");
  flags2.set_key_root_relative("../../root2");
  flags2.set_key_root_relative_remapped(true);

  const HandledSource source1(
      "# 1 \"../../root1/a.cc\"\nint main() {}\n"_l);
  const HandledSource source2(
      "# 1 \"../../root2/a.cc\"\nint main() {}\n"_l);
  EXPECT_EQ(CompilationDaemon::GenerateHash(flags1, source1, {}),
            CompilationDaemon::GenerateHash(flags2, source2, {}));

  const HandledSource source3(
      "# 1 \"../../root2/a.cc\"\nint main() { return 1; }\n"_l);
  EXPECT_NE(CompilationDaemon::GenerateHash(flags1, source1, {}),
            CompilationDaemon::GenerateHash(flags2, source3, {}));
}

TEST(CompilationDaemonTest, DirectKeyKeepsRootInOtherFlags) {
  auto Flags = [](const String& root, const String& define) {
    base::proto::Flags flags;
    flags.set_input(root + "/src/a.cc");
    flags.set_key_root(root);
    flags.set_key_root_relative("..");
    flags.set_key_root_remapped(true);
    auto* include = flags.add_non_cached();
    include->set_index(1);
    include->add_values("-I");
    include->add_values(root + "/include");
    flags.add_non_cached()->add_values("-I" + root + "/gen");
    flags.add_other()->add_values("-fmacro-prefix-map=" + root + "/=");
    flags.add_non_cached()->add_values("-DDATA=\"" + define + "/data\"");
    return flags;
  };

  // The paths, that the compiler remaps, don't matter.
  EXPECT_EQ(CompilationDaemon::DirectCommandLine("/build",
                                                 Flags("/root1", "/data")),
            CompilationDaemon::DirectCommandLine("/build",
                                                 Flags("/root2", "/data")));
  // The definition gets into the object as is.
  EXPECT_NE(CompilationDaemon::DirectCommandLine("/build",
                                                 Flags("/root1", "/root1")),
            CompilationDaemon::DirectCommandLine("/build",
                                                 Flags("/root2", "/root2")));
}

TEST(CompilationDaemonTest, DirectCommandLineOfSourcesNextToCurrentDir) {
  auto Flags = [](const String& input) {
    base::proto::Flags flags;
    flags.set_input(input);
    return flags;
  };

  // The directories with the same prefix as the current one aren't inside it.
  EXPECT_NE(CompilationDaemon::DirectCommandLine("/p/out",
                                                 Flags("/p/out1/gen.cc")),
            CompilationDaemon::DirectCommandLine("/p/out",
                                                 Flags("/p/out2/gen.cc")));
  EXPECT_NE(CompilationDaemon::DirectCommandLine("/p/out",
                                                 Flags("/p/out_a/gen.cc")),
            CompilationDaemon::DirectCommandLine("/p/out", Flags("a/gen.cc")));
  EXPECT_EQ(CompilationDaemon::DirectCommandLine("/p/out",
                                                 Flags("/p/out/a/gen.cc")),
            CompilationDaemon::DirectCommandLine("/p/out", Flags("a/gen.cc")));
}

TEST(CompilationDaemonTest, DepsWithWrappedTargets) {
  base::proto::Flags flags;
  flags.set_deps_target(
      "obj/a/some/long/path/foo.o obj/a/some/long/path/foo.o.stamp");
  // The compiler wraps the long line of the targets.
  const auto deps = CompilationDaemon::NormalizeDeps(
      "obj/a/some/long/path/foo.o \\\n"
      "  obj/a/some/long/path/foo.o.stamp: a.cc \\\n  a.h\n"_l,
      flags);

  flags.set_deps_target("obj/b/foo.o");
  EXPECT_EQ("obj/b/foo.o: a.cc \\\n  a.h\n"_l,
            CompilationDaemon::RestoreDeps(deps, flags));
}

TEST(CompilationDaemonTest, DepsOfAnotherRoot) {
  auto Flags = [](const String& root) {
    base::proto::Flags flags;
    flags.set_deps_target("obj/a.o");
    flags.set_key_root("/home/" + root);
    flags.set_key_root_relative("../../" + root);
    return flags;
  };

  // The paths of the dependency file are the ones of its root - after the hit
  // from another root too.
  const auto deps = CompilationDaemon::NormalizeDeps(
      "obj/a.o: ../../root1/a.cc /home/root1/inc/b.h /usr/include/c.h \\\n"
      " gen/d.h\n"_l,
      Flags("root1"));
  EXPECT_EQ(
      "obj/a.o: ../../root2/a.cc /home/root2/inc/b.h /usr/include/c.h \\\n"
      " gen/d.h\n"_l,
      CompilationDaemon::RestoreDeps(deps, Flags("root2")));
}

TEST(CompilationDaemonTest, TimeTraceOfAnotherRoot) {
  auto Flags = [](const String& root) {
    base::proto::Flags flags;
    flags.set_deps_target("obj/a.o");
    flags.set_key_root("/home/" + root);
    flags.set_key_root_relative("../../" + root);
    return flags;
  };

  // The paths of the trace are the ones of its root - after the hit from
  // another root too.
  const auto trace = CompilationDaemon::NormalizeTimeTrace(
      R"({"traceEvents":[{"name":"Source","args":{"detail":"/home/root1/a.h"}},)"
      R"({"name":"Source","args":{"detail":"../../root1/b.h"}},)"
      R"({"name":"Source","args":{"detail":"/usr/include/c.h"}}]})"_l,
      Flags("root1"));
  EXPECT_EQ(
      R"({"traceEvents":[{"name":"Source","args":{"detail":"/home/root2/a.h"}},)"
      R"({"name":"Source","args":{"detail":"../../root2/b.h"}},)"
      R"({"name":"Source","args":{"detail":"/usr/include/c.h"}}]})"_l,
      CompilationDaemon::RestoreTimeTrace(trace, Flags("root2")));
}

TEST(CompilationDaemonTest, TimeTraceOptions) {
  auto Flags = [](std::initializer_list<const char*> args) {
    base::proto::Flags flags;
    for (const char* arg : args) {
      flags.add_ignored()->add_values(arg);
    }
    return flags;
  };

  // The place of the trace doesn't change it.
  EXPECT_EQ(CompilationDaemon::TimeTraceOptions(Flags({"-ftime-trace"})),
            CompilationDaemon::TimeTraceOptions(
                Flags({"-ftime-trace=a.json"})));
  EXPECT_NE(CompilationDaemon::TimeTraceOptions(Flags({"-ftime-trace"})),
            CompilationDaemon::TimeTraceOptions(Flags(
                {"-ftime-trace", "-ftime-trace-granularity=100"})));
  EXPECT_NE(CompilationDaemon::TimeTraceOptions(Flags({"-ftime-trace"})),
            CompilationDaemon::TimeTraceOptions(
                Flags({"-ftime-trace", "-ftime-trace-verbose"})));
}

TEST(CompilationDaemonTest, ModificationRightAfterStart) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.cc", "int a;"_l));
  base::proto::Local message;
  message.set_current_dir(temp_dir.path());
  message.mutable_flags()->set_input("a.cc");

  // The file system may take the time from the coarse clock: the modification
  // right after the start - within the same tick - is seen too.
  ui32 missed = 0;
  for (int i = 0; i < 200; ++i) {
    const auto start = CompilationDaemon::FileSystemTime();
    ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.cc",
                                  Immutable(std::to_string(i))));
    if (CompilationDaemon::InputsUnchangedSince(&message, Immutable(), start)) {
      ++missed;
    }
  }
  EXPECT_EQ(0u, missed);
}

TEST(CompilationDaemonTest, PreprocessedInputs) {
  List<String> inputs;
  EXPECT_TRUE(CompilationDaemon::PreprocessedInputs(
      "# 1 \"a.cc\"\n"
      "# 1 \"<built-in>\" 1\n"
      "# 1 \"<command line>\" 1\n"
      "# 1 \"a.cc\" 2\n"
      "#pragma once\n"
      "# 1 \"dir with space/b\\\"c.h\" 1 3\n"
      "int b;\n"
      "# 2 \"a.cc\" 2\n"
      "# 1 \"d\\\\e.h\" 1\n"
      "# 1 \"f\\303\\251.h\" 1\n"
      "  # 5 \"g.h\"\n"
      "# 3 \"a.cc\" 2"_l,
      inputs));
  EXPECT_EQ((List<String>{"a.cc", "dir with space/b\"c.h", "d\\e.h",
                          "f\xc3\xa9.h"}),
            inputs);

  // Without the line markers, e.g. with "-P", the inputs are unknown.
  inputs.clear();
  EXPECT_FALSE(CompilationDaemon::PreprocessedInputs("int a;\n"_l, inputs));
}

TEST(CompilationDaemonTest, InputsUnchangedSince) {
  const base::TemporaryDir temp_dir;
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.cc", "int a;"_l));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.h", "int b;"_l));

  base::proto::Local message;
  message.set_current_dir(temp_dir.path());
  message.mutable_flags()->set_input("a.cc");
  const auto deps = "a.o: a.cc a.h"_l;

  // The coarse clock ticks at least every few milliseconds.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto start = CompilationDaemon::FileSystemTime();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_TRUE(CompilationDaemon::InputsUnchangedSince(&message, deps, start));

  ASSERT_TRUE(base::File::Write(temp_dir.path() / "a.h", "int c;"_l));
  EXPECT_FALSE(CompilationDaemon::InputsUnchangedSince(&message, deps, start));
  // The modifications shortly before the start look modified too.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(CompilationDaemon::InputsUnchangedSince(
      &message, deps, CompilationDaemon::FileSystemTime()));

  ASSERT_TRUE(base::File::Delete(temp_dir.path() / "a.cc"));
  EXPECT_FALSE(CompilationDaemon::InputsUnchangedSince(
      &message, deps, CompilationDaemon::FileSystemTime()));
}

}  // namespace daemon
}  // namespace dist_clang
