#include "base/constants.h"
#include "base/file_utils.h"
#include "base/process_impl.h"
#include "base/test_process.h"
#include "base/temporary_dir.h"
#include "client/clang.h"
#include "client/clang_flag_set.h"
#include "gtest/gtest.h"
#include "net/network_service_impl.h"
#include "net/test_network_service.h"
#include "proto/remote.pb.h"

namespace dist_clang {
namespace client {

namespace {

// NOTICE: if changing something in these strings, make sure to apply the same
//         changes to the tests below.

// It's a possible output of the command:
// `cd /tmp; clang++ -### -c /tmp/test.cc`
const std::string clang_cc_output =
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

// It's a possible output of the command:
// `cd /tmp; clang++ -### -E -c /tmp/test.cc`
const std::string clang_pp_output =
    "clang version 3.4 (...) (...)\n"
    "Target: x86_64-unknown-linux-gnu\n"
    "Thread model: posix\n"
    " \"/usr/bin/clang\" \"-cc1\""
    " \"-triple\" \"x86_64-unknown-linux-gnu\""
    " \"-E\""
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
    " \"-coverage-file\" \"/tmp/-\""
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
    " \"-o\" \"-\""
    " \"-x\" \"c++\""
    " \"/tmp/test.cc\"\n";

}  // namespace

TEST(ClangFlagSetTest, SimpleInput) {
  std::string version;
  const std::string expected_version = "clang version 3.4 (...) (...)";
  ClangFlagSet::StringList input;
  const ClangFlagSet::StringList expected_input = {
    "",
    "/usr/bin/clang", "-cc1",
    "-triple", "x86_64-unknown-linux-gnu",
    "-emit-obj",
    "-mrelax-all",
    "-disable-free",
    "-main-file-name", "test.cc",
    "-mrelocation-model", "static",
    "-mdisable-fp-elim",
    "-fmath-errno",
    "-masm-verbose",
    "-mconstructor-aliases",
    "-munwind-tables",
    "-fuse-init-array",
    "-target-cpu", "x86-64",
    "-target-linker-version", "2.23.2",
    "-coverage-file", "/tmp/test.o",
    "-resource-dir", "/usr/lib/clang/3.4",
    "-internal-isystem", "/usr/include/c++/4.8.2",
    "-internal-isystem", "/usr/local/include",
    "-internal-isystem", "/usr/lib/clang/3.4/include",
    "-internal-externc-isystem", "/include",
    "-internal-externc-isystem", "/usr/include",
    "-fdeprecated-macro",
    "-fdebug-compilation-dir", "/tmp",
    "-ferror-limit", "19",
    "-fmessage-length", "213",
    "-mstackrealign",
    "-fobjc-runtime=gcc",
    "-fcxx-exceptions",
    "-fexceptions",
    "-fdiagnostics-show-option",
    "-fcolor-diagnostics",
    "-vectorize-slp",
    "-o", "test.o",
    "-x", "c++",
    "/tmp/test.cc"
  };
  auto it = expected_input.begin();

  EXPECT_TRUE(ClangFlagSet::ParseClangOutput(clang_cc_output, &version, input));
  EXPECT_EQ(expected_version, version);
  for (const auto& value: input) {
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
  expected_flags.add_other()->assign("-emit-obj");
  expected_flags.add_other()->assign("-mrelax-all");
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

  proto::Flags actual_flags;
  actual_flags.mutable_compiler()->set_version(version);
  ASSERT_EQ(ClangFlagSet::COMPILE,
            ClangFlagSet::ProcessFlags(input, &actual_flags));
  ASSERT_EQ(expected_flags.SerializeAsString(),
            actual_flags.SerializeAsString());
}

class ClientTest: public ::testing::Test {
  public:
    virtual void SetUp() override {
      using Service = net::TestNetworkService;

      {
        auto factory = net::NetworkService::SetFactory<Service::Factory>();
        factory->CallOnCreate([this](Service* service) {
          service->CountConnectAttempts(&connect_count);
          service->CallOnConnect([this](net::EndPointPtr, std::string* error) {
            if (!do_connect) {
              if (error) {
                error->assign("Test service rejects connection intentionally");
              }
              return Service::TestConnectionPtr();
            }

            auto connection =
                Service::TestConnectionPtr(new net::TestConnection);
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
          process->CallOnRun([this, process](unsigned timeout,
                                             const std::string& input,
                                             std::string* error) {
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
    uint send_count = 0, read_count = 0, connect_count = 0,
        connections_created = 0, run_count = 0;
    std::function<void(net::TestConnection*)> connect_callback =
        EmptyLambda<>();
    std::function<void(base::TestProcess*)> run_callback = EmptyLambda<>();
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

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), std::string()));
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
    for (size_t i = 1; i < argc; ++i, ++it) {
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
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(2u, run_count);
}

TEST_F(ClientTest, CannotReadMessage) {
  const std::string test_file_base = "test";
  const auto test_file_name = test_file_base + ".cc";
  const auto test_file = "/tmp/" + test_file_name;

  connect_callback = [&](net::TestConnection* connection) {
    connection->AbortOnRead();
    connection->CallOnSend([&](const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(proto::Execute::extension));

      const auto& extension = message.GetExtension(proto::Execute::extension);
      EXPECT_FALSE(extension.remote());
      EXPECT_EQ(base::GetCurrentDir(), extension.current_dir());
      ASSERT_TRUE(extension.has_cc_flags());
      ASSERT_TRUE(extension.has_pp_flags());

      const auto& cc_flags = extension.cc_flags();
      ASSERT_TRUE(cc_flags.has_compiler());
      // TODO: check compiler version and path.
      EXPECT_EQ(test_file_base + ".o", cc_flags.output());
      EXPECT_EQ(test_file, cc_flags.input());
      EXPECT_EQ("c++", cc_flags.language());

      {
        const auto& other = cc_flags.other();
        auto begin = other.begin();
        auto end = other.end();
        EXPECT_NE(end, std::find(begin, end, "-cc1"));
        EXPECT_NE(end, std::find(begin, end, "-triple"));
        EXPECT_NE(end, std::find(begin, end, "-emit-obj"));
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

      const auto& pp_flags = extension.pp_flags();
      ASSERT_TRUE(pp_flags.has_compiler());
      // TODO: check compiler version and path.
      EXPECT_FALSE(pp_flags.has_output());
      EXPECT_EQ(test_file, pp_flags.input());
      EXPECT_EQ("c++", pp_flags.language());

      {
        const auto& other = pp_flags.other();
        auto begin = other.begin();
        auto end = other.end();
        EXPECT_NE(end, std::find(begin, end, "-cc1"));
        EXPECT_NE(end, std::find(begin, end, "-triple"));
        EXPECT_NE(end, std::find(begin, end, "-E"));
        EXPECT_NE(end, std::find(begin, end, "-target-cpu"));
        EXPECT_NE(end, std::find(begin, end, "-target-linker-version"));
      }

      {
        const auto& non_cached = pp_flags.non_cached();
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
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(2u, run_count);
}

TEST_F(ClientTest, ReadMessageWithoutStatus) {
  const int argc = 3;
  const char* expected_exec_path = "clang++";
  const char* argv[] = {expected_exec_path, "-c", "/tmp/test.cc", nullptr};

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(2u, run_count);
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
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(2u, run_count);
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
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_FALSE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(2u, run_count);
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
    if (run_count == 1) {
      process->stderr_ = clang_cc_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
    else if (run_count == 2) {
      process->stderr_ = clang_pp_output;
      auto it = process->args_.begin();
      EXPECT_EQ(expected_exec_path, process->exec_path_);
      EXPECT_TRUE(process->cwd_path_.empty());
      EXPECT_EQ("-###", *(it++));
      EXPECT_EQ("-E", *(it++));
      for (size_t i = 1; i < argc; ++i, ++it) {
        EXPECT_EQ(argv[i], *it);
      }
    }
  };

  EXPECT_EXIT(client::DoMain(argc, argv, std::string(), "clang++"),
              ::testing::ExitedWithCode(1), ".*");
}

}  // namespace client
}  // namespace dist_clang
