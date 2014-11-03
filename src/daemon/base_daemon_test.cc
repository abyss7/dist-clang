#include <daemon/base_daemon.h>

#include <base/process.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace daemon {

TEST(BaseDaemonTest, CreateProcessFromFlags) {
  const List<String> expected_args = {
      "-cc1", "-emit-obj", "-I.", "-load", "/usr/lib/libplugin.so",
      "-dependency-file", "some_deps_file", "-x", "c++", "-o", "test.o",
      "test.cc",
  };
  const ui32 expected_user_id = 1u;

  proto::Flags flags;
  flags.mutable_compiler()->set_path("/usr/bin/clang");
  flags.mutable_compiler()->add_plugins()->set_path("/usr/lib/libplugin.so");
  flags.add_cc_only()->assign("-mrelax-all");
  flags.add_non_cached()->assign("-I.");
  flags.add_other()->assign("-cc1");
  flags.set_action("-emit-obj");
  flags.set_input("test.cc");
  flags.set_output("test.o");
  flags.set_language("c++");
  flags.set_deps_file("some_deps_file");

  {
    base::ProcessPtr process =
        BaseDaemon::CreateProcess(flags, expected_user_id);
    auto it = expected_args.begin();
    ASSERT_EQ(expected_args.size(), process->args_.size());
    for (const auto& arg : process->args_) {
      EXPECT_EQ(*(it++), arg);
    }
    EXPECT_EQ("/usr/bin/clang", process->exec_path_);
    EXPECT_EQ(expected_user_id, process->uid_);
  }

  {
    base::ProcessPtr process = BaseDaemon::CreateProcess(flags);
    auto it = expected_args.begin();
    ASSERT_EQ(expected_args.size(), process->args_.size());
    for (const auto& arg : process->args_) {
      EXPECT_EQ(*(it++), arg);
    }
    EXPECT_EQ("/usr/bin/clang", process->exec_path_);
    EXPECT_EQ(base::Process::SAME_UID, process->uid_);
  }
}

}  // namespace daemon
}  // namespace dist_clang
