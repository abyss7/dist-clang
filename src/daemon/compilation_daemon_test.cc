#include <daemon/compilation_daemon.h>

#include <base/process.h>

#include <gtest/gtest.h>

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

}  // namespace daemon
}  // namespace dist_clang
