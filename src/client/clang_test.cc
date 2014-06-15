#include <base/constants.h>
#include <base/file_utils.h>
#include <base/process_impl.h>
#include <base/test_process.h>
#include <base/temporary_dir.h>
#include <client/clang.h>
#include <client/clang_flag_set.h>
#include <net/network_service_impl.h>
#include <net/test_network_service.h>
#include <proto/remote.pb.h>

#include <third_party/gtest/public/gtest/gtest.h>

namespace dist_clang {
namespace client {

namespace {

// NOTICE: if changing something in these strings, make sure to apply the same
//         changes to the tests below.

// It's a possible output of the command:
// `cd /tmp; clang++ -### -c /tmp/test.cc`
const String clang_cc_output =
    "clang version 3.4 (...) (...)\n"
    "Target: x86_64-unknown-linux-gnu\n"
    "Thread model: posix\n"
    " \"/usr/bin/clang\" \"-cc1\""
    " \"-triple\" \"x86_64-unknown-linux-gnu\""
    " \"-emit-obj\""
    " \"-mrelax-all\""
    " \"-disable-free\""
    " \"-main-file-name\" \"test.cc\""
    " \"-mrelocation-model\" \"static\""
    " \"-mdisable-fp-elim\""
    " \"-fmath-errno\""
    " \"-masm-verbose\""
    " \"-mconstructor-aliases\""
    " \"-munwind-tables\""
    " \"-fuse-init-array\""
    " \"-target-cpu\" \"x86-64\""
    " \"-target-linker-version\" \"2.23.2\""
    " \"-coverage-file\" \"/tmp/test.o\""
    " \"-resource-dir\" \"/usr/lib/clang/3.4\""
    " \"-internal-isystem\" \"/usr/include/c++/4.8.2\""
    " \"-internal-isystem\" \"/usr/local/include\""
    " \"-internal-isystem\" \"/usr/lib/clang/3.4/include\""
    " \"-internal-externc-isystem\" \"/include\""
    " \"-internal-externc-isystem\" \"/usr/include\""
    " \"-fdeprecated-macro\""
    " \"-fdebug-compilation-dir\" \"/tmp\""
    " \"-ferror-limit\" \"19\""
    " \"-fmessage-length\" \"213\""
    " \"-mstackrealign\""
    " \"-fobjc-runtime=gcc\""
    " \"-fcxx-exceptions\""
    " \"-fexceptions\""
    " \"-fdiagnostics-show-option\""
    " \"-fcolor-diagnostics\""
    " \"-vectorize-slp\""
    " \"-o\" \"test.o\""
    " \"-x\" \"c++\""
    " \"/tmp/test.cc\"\n";
}  // namespace

TEST(ClangFlagSetTest, SimpleInput) {
  String version;
  const String expected_version = "clang version 3.4 (...) (...)";
  FlagSet::CommandList input;
  const List<String> expected_input = {
      "",                          "/usr/bin/clang",
      "-cc1",                      "-triple",
      "x86_64-unknown-linux-gnu",  "-emit-obj",
      "-mrelax-all",               "-disable-free",
      "-main-file-name",           "test.cc",
      "-mrelocation-model",        "static",
      "-mdisable-fp-elim",         "-fmath-errno",
      "-masm-verbose",             "-mconstructor-aliases",
      "-munwind-tables",           "-fuse-init-array",
      "-target-cpu",               "x86-64",
      "-target-linker-version",    "2.23.2",
      "-coverage-file",            "/tmp/test.o",
      "-resource-dir",             "/usr/lib/clang/3.4",
      "-internal-isystem",         "/usr/include/c++/4.8.2",
      "-internal-isystem",         "/usr/local/include",
      "-internal-isystem",         "/usr/lib/clang/3.4/include",
      "-internal-externc-isystem", "/include",
      "-internal-externc-isystem", "/usr/include",
      "-fdeprecated-macro",        "-fdebug-compilation-dir",
      "/tmp",                      "-ferror-limit",
      "19",                        "-fmessage-length",
      "213",                       "-mstackrealign",
      "-fobjc-runtime=gcc",        "-fcxx-exceptions",
      "-fexceptions",              "-fdiagnostics-show-option",
      "-fcolor-diagnostics",       "-vectorize-slp",
      "-o",                        "test.o",
      "-x",                        "c++",
      "/tmp/test.cc"};
  auto it = expected_input.begin();

  EXPECT_TRUE(FlagSet::ParseClangOutput(clang_cc_output, &version, input));
  EXPECT_EQ(expected_version, version);
  for (const auto& value : *input.begin()) {
    ASSERT_EQ(*(it++), value);
  }

  proto::Flags expected_flags;
  expected_flags.mutable_compiler()->set_path("/usr/bin/clang");
  expected_flags.mutable_compiler()->set_version(expected_version);
  expected_flags.set_output("test.o");
  expected_flags.set_input("/tmp/test.cc");
  expected_flags.set_language("c++");
  expected_flags.add_other()->assign("-cc1");
  expected_flags.add_other()->assign("-triple");
  expected_flags.add_other()->assign("x86_64-unknown-linux-gnu");
  expected_flags.add_other()->assign("-disable-free");
  expected_flags.add_other()->assign("-mrelocation-model");
  expected_flags.add_other()->assign("static");
  expected_flags.add_other()->assign("-mdisable-fp-elim");
  expected_flags.add_other()->assign("-fmath-errno");
  expected_flags.add_other()->assign("-masm-verbose");
  expected_flags.add_other()->assign("-mconstructor-aliases");
  expected_flags.add_other()->assign("-munwind-tables");
  expected_flags.add_other()->assign("-fuse-init-array");
  expected_flags.add_other()->assign("-target-cpu");
  expected_flags.add_other()->assign("x86-64");
  expected_flags.add_other()->assign("-target-linker-version");
  expected_flags.add_other()->assign("2.23.2");
  expected_flags.add_other()->assign("-fdeprecated-macro");
  expected_flags.add_other()->assign("-fmessage-length");
  expected_flags.add_other()->assign("213");
  expected_flags.add_other()->assign("-mstackrealign");
  expected_flags.add_other()->assign("-fobjc-runtime=gcc");
  expected_flags.add_other()->assign("-fcxx-exceptions");
  expected_flags.add_other()->assign("-fexceptions");
  expected_flags.add_other()->assign("-fdiagnostics-show-option");
  expected_flags.add_other()->assign("-fcolor-diagnostics");
  expected_flags.add_other()->assign("-vectorize-slp");
  expected_flags.add_non_cached()->assign("-main-file-name");
  expected_flags.add_non_cached()->assign("test.cc");
  expected_flags.add_non_cached()->assign("-coverage-file");
  expected_flags.add_non_cached()->assign("/tmp/test.o");
  expected_flags.add_non_cached()->assign("-resource-dir");
  expected_flags.add_non_cached()->assign("/usr/lib/clang/3.4");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/include/c++/4.8.2");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/local/include");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/lib/clang/3.4/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/usr/include");
  expected_flags.add_non_cached()->assign("-fdebug-compilation-dir");
  expected_flags.add_non_cached()->assign("/tmp");
  expected_flags.add_non_cached()->assign("-ferror-limit");
  expected_flags.add_non_cached()->assign("19");
  expected_flags.add_cc_only()->assign("-mrelax-all");
  expected_flags.set_action("-emit-obj");

  proto::Flags actual_flags;
  actual_flags.mutable_compiler()->set_version(version);
  ASSERT_EQ(FlagSet::COMPILE,
            FlagSet::ProcessFlags(*input.begin(), &actual_flags));
  ASSERT_EQ(expected_flags.SerializeAsString(),
            actual_flags.SerializeAsString());
}

TEST(ClangFlagSetTest, MultipleCommands) {
  String version;
  const String expected_version = "clang version 3.4 (...) (...)";
  FlagSet::CommandList input;
  const String clang_multi_output =
      "clang version 3.4 (...) (...)\n"
      "Target: x86_64-unknown-linux-gnu\n"
      "Thread model: posix\n"
      " \"/usr/bin/clang\""
      " \"-emit-obj\""
      " \"test.cc\"\n"
      " \"/usr/bin/objcopy\""
      " \"something\""
      " \"some_file\"\n";
  const List<String> expected_input1 = {"",          "/usr/bin/clang",
                                        "-emit-obj", "test.cc", };
  const List<String> expected_input2 = {"",          "/usr/bin/objcopy",
                                        "something", "some_file", };

  EXPECT_TRUE(FlagSet::ParseClangOutput(clang_multi_output, &version, input));
  EXPECT_EQ(expected_version, version);

  ASSERT_EQ(2u, input.size());

  auto it1 = expected_input1.begin();
  for (const auto& value : input.front()) {
    EXPECT_EQ(*(it1++), value);
  }

  auto it2 = expected_input2.begin();
  for (const auto& value : input.back()) {
    EXPECT_EQ(*(it2++), value);
  }

  proto::Flags actual_flags;
  actual_flags.mutable_compiler()->set_version(version);
  ASSERT_EQ(FlagSet::COMPILE,
            FlagSet::ProcessFlags(input.front(), &actual_flags));
  ASSERT_EQ(FlagSet::UNKNOWN,
            FlagSet::ProcessFlags(input.back(), &actual_flags));
}

class ClientTest : public ::testing::Test {
 public:
  virtual void SetUp() override {
    using Service = net::TestNetworkService;

    {
      auto factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        service->CountConnectAttempts(&connect_count);
        service->CallOnConnect([this](net::EndPointPtr, String* error) {
          if (!do_connect) {
            if (error) {
              error->assign("Test service rejects connection intentionally");
            }
            return Service::TestConnectionPtr();
          }

          auto connection = Service::TestConnectionPtr(new net::TestConnection);
          connection->CountSendAttempts(&send_count);
          connection->CountReadAttempts(&read_count);
          weak_ptr = connection->shared_from_this();
          ++connections_created;
          connect_callback(connection.get());

          return connection;
        });
      });
    }

    {
      auto factory = base::Process::SetFactory<base::TestProcess::Factory>();
      factory->CallOnCreate([this](base::TestProcess* process) {
        process->CountRuns(&run_count);
        process->CallOnRun([this, process](ui32 timeout, const String& input,
                                           String* error) {
          run_callback(process);

          if (!do_run) {
            if (error) {
              error->assign("Test process fails to run intentionally");
            }
            return false;
          }

          return true;
        });
      });
    }
  }

 protected:
  bool do_connect = true, do_run = true;
  net::ConnectionWeakPtr weak_ptr;
  ui32 send_count = 0, read_count = 0, connect_count = 0,
       connections_created = 0, run_count = 0;
  Fn<void(net::TestConnection*)> connect_callback = EmptyLambda<>();
  Fn<void(base::TestProcess*)> run_callback = EmptyLambda<>();
};

TEST_F(ClientTest, NoConnection) {
  const int argc = 3;
  const char* argv[] = {"a", "b", "c", nullptr};

  do_connect = false;
  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", "clang_path"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(0u, connections_created);
  EXPECT_EQ(0u, run_count);
}

TEST_F(ClientTest, NoEnvironmentVariable) {
  const int argc = 3;
  const char* argv[] = {"clang++", "-c", "/tmp/test.cc", nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, String(), String()));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
  EXPECT_EQ(0u, run_count);
}

TEST_F(ClientTest, NoInputFile) {
  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/qwerty", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ =
        "clang version 3.4 (...) (...)\n"
        "Target: x86_64-unknown-linux-gnu\n"
        "Thread model: posix\n"
        "clang: error: no such file or directory: '/tmp/qwerty'\n";

    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", expected_exec_path));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, CannotSendMessage) {
  connect_callback = [](net::TestConnection* connection) {
    connection->AbortOnSend();
  };

  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, CannotReadMessage) {
  const String test_file_base = "test";
  const auto test_file_name = test_file_base + ".cc";
  const auto test_file = "/tmp/" + test_file_name;

  connect_callback = [&](net::TestConnection* connection) {
    connection->AbortOnRead();
    connection->CallOnSend([&](const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(proto::Execute::extension));

      const auto& extension = message.GetExtension(proto::Execute::extension);
      EXPECT_FALSE(extension.remote());
      EXPECT_EQ(base::GetCurrentDir(), extension.current_dir());
      ASSERT_TRUE(extension.has_flags());

      const auto& cc_flags = extension.flags();
      ASSERT_TRUE(cc_flags.has_compiler());
      // TODO: check compiler version and path.
      EXPECT_EQ(test_file_base + ".o", cc_flags.output());
      EXPECT_EQ(test_file, cc_flags.input());
      EXPECT_EQ("c++", cc_flags.language());
      EXPECT_EQ("-emit-obj", cc_flags.action());

      {
        const auto& other = cc_flags.other();
        auto begin = other.begin();
        auto end = other.end();
        EXPECT_NE(end, std::find(begin, end, "-cc1"));
        EXPECT_NE(end, std::find(begin, end, "-triple"));
        EXPECT_NE(end, std::find(begin, end, "-target-cpu"));
        EXPECT_NE(end, std::find(begin, end, "-target-linker-version"));
      }

      {
        const auto& non_cached = cc_flags.non_cached();
        auto begin = non_cached.begin();
        auto end = non_cached.end();
        EXPECT_NE(end, std::find(begin, end, "-main-file-name"));
        EXPECT_NE(end, std::find(begin, end, test_file_name));
        EXPECT_NE(end, std::find(begin, end, "-coverage-file"));
        EXPECT_NE(end, std::find(begin, end, "-resource-dir"));
        EXPECT_NE(end, std::find(begin, end, "-internal-isystem"));
        EXPECT_NE(end, std::find(begin, end, "-internal-externc-isystem"));
      }
    });
  };

  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", test_file.c_str(), nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, ReadMessageWithoutStatus) {
  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, ReadMessageWithBadStatus) {
  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::INCONSEQUENT);
    });
  };

  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, SuccessfulCompilation) {
  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::OK);
    });
  };

  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_FALSE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, run_count);
}

TEST_F(ClientTest, FailedCompilation) {
  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::EXECUTION);
    });
  };

  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    process->stderr_ = clang_cc_output;
    auto it = process->args_.begin();
    EXPECT_EQ(expected_exec_path, process->exec_path_);
    EXPECT_TRUE(process->cwd_path_.empty());
    EXPECT_EQ("-###", *(it++));
    for (int i = 1; i < argc; ++i, ++it) {
      EXPECT_EQ(argv[i], *it);
    }
  };

  EXPECT_EXIT(client::DoMain(argc, argv, String(), "clang++"),
              ::testing::ExitedWithCode(1), ".*");
}

TEST_F(ClientTest, DISABLED_MultipleCommands_OneFails) {
  // TODO: implement this.
}

TEST_F(ClientTest, DISABLED_MultipleCommands_Successful) {
  // TODO: implement this.
}

}  // namespace client
}  // namespace dist_clang
