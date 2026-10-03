#include <daemon/compilation_daemon.h>

#include <base/file/file.h>
#include <base/process.h>
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
}

TEST(CompilationDaemonTest, NormalizePaths) {
  base::proto::Flags flags;
  EXPECT_EQ("-I/src/a", CompilationDaemon::NormalizePaths("-I/src/a", flags));

  flags.set_key_root("/home/user/src");
  flags.set_key_root_relative("../src");
  EXPECT_EQ(
      "-I<ROOT>/a -I<ROOT> -I<ROOT>/b -I../src.old/c \"<ROOT>\" "
      "-isystem <ROOT>/d /other/home/user/src/e x/../src/f "
      "-fmacro-prefix-map=<ROOT>/=",
      CompilationDaemon::NormalizePaths(
          "-I/home/user/src/a -I/home/user/src -I../src/b -I../src.old/c "
          "\"../src\" -isystem /home/user/src/d /other/home/user/src/e "
          "x/../src/f -fmacro-prefix-map=../src/=",
          flags));
}

TEST(CompilationDaemonTest, NormalizeLineMarkers) {
  base::proto::Flags flags;
  flags.set_key_root("/root1");
  flags.set_key_root_relative("../root1");

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
  flags2.add_other()->add_values("-fmacro-prefix-map=../../root2/=");
  flags2.set_key_root("/home/root2");
  flags2.set_key_root_relative("../../root2");

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
  EXPECT_TRUE(CompilationDaemon::InputsUnchangedSince(
      &message, deps, CompilationDaemon::FileSystemTime()));

  ASSERT_TRUE(base::File::Delete(temp_dir.path() / "a.cc"));
  EXPECT_FALSE(CompilationDaemon::InputsUnchangedSince(
      &message, deps, CompilationDaemon::FileSystemTime()));
}

}  // namespace daemon
}  // namespace dist_clang
