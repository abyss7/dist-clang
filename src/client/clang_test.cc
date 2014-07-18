#include <base/constants.h>
#include <base/file_utils.h>
#include <base/process_impl.h>
#include <base/test_process.h>
#include <base/temporary_dir.h>
#include <client/clang.h>
#include <client/command.h>
#include <net/network_service_impl.h>
#include <net/test_network_service.h>
#include <proto/remote.pb.h>

#include <third_party/gtest/public/gtest/gtest.h>
#include <third_party/libcxx/exported/include/regex>

namespace dist_clang {
namespace client {

TEST(CommandTest, NonExistentInput) {
  const int argc = 3;
  const char* argv[] = {"clang++", "-c", "/tmp/some_random.cc", nullptr};

  List<Command> commands;
  ASSERT_FALSE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_TRUE(commands.empty());
}

TEST(CommandTest, MissingArgument) {
  const int argc = 4;
  const char* argv[] = {"clang++", "-I", "-c", "/tmp/some_random.cc", nullptr};

  List<Command> commands;
  ASSERT_FALSE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_TRUE(commands.empty());
}

TEST(CommandTest, UnknownArgument) {
  const int argc = 4;
  const char* argv[] = {"clang++", "-12", "-c", "/tmp/some_random.cc", nullptr};

  List<Command> commands;
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

  const String temp_input = base::CreateTempFile(".cc");
  const String expected_output = "/tmp/output.o";
  List<Pair<std::regex, String>> expected_regex;
  expected_regex.push_back(rep("-cc1"));
  expected_regex.push_back(rep("-triple [a-z0-9_]+-[a-z0-9_]+-[a-z0-9]+"));
  expected_regex.push_back(rep("-emit-obj"));
  expected_regex.push_back(rep("-mrelax-all"));
  expected_regex.push_back(rep("-disable-free"));
  expected_regex.push_back(rep("-main-file-name clangd-[a-zA-Z0-9]+\\.cc"));
  expected_regex.push_back(rep("-mrelocation-model static"));
  expected_regex.push_back(rep("-mdisable-fp-elim"));
  expected_regex.push_back(rep("-fmath-errno"));
  expected_regex.push_back(rep("-masm-verbose"));
  expected_regex.push_back(rep("-mconstructor-aliases"));
  expected_regex.push_back(rep("-munwind-tables"));
  expected_regex.push_back(rep("-fuse-init-array"));
  expected_regex.push_back(rep("-target-cpu [a-z0-9_]+"));
  expected_regex.push_back(rep("-target-linker-version [0-9.]+"));
  expected_regex.push_back(rep2("-coverage-file " + expected_output));
  expected_regex.push_back(rep("-resource-dir"));
  expected_regex.push_back(rep("-internal-isystem"));
  expected_regex.push_back(rep("-internal-externc-isystem"));
  expected_regex.push_back(rep("-fdeprecated-macro"));
  expected_regex.push_back(rep("-fdebug-compilation-dir"));
  expected_regex.push_back(rep("-ferror-limit [0-9]+"));
  expected_regex.push_back(rep("-fmessage-length [0-9]+"));
  expected_regex.push_back(rep("-mstackrealign"));
  expected_regex.push_back(rep("-fobjc-runtime="));
  expected_regex.push_back(rep("-fcxx-exceptions"));
  expected_regex.push_back(rep("-fexceptions"));
  expected_regex.push_back(rep("-fdiagnostics-show-option"));
  expected_regex.push_back(rep2("-o " + expected_output));
  expected_regex.push_back(rep("-x c++"));
  expected_regex.push_back(rep2(temp_input));

  const char* argv[] = {"clang++", "-c",                    temp_input.c_str(),
                        "-o",      expected_output.c_str(), nullptr};
  const int argc = 5;

  List<Command> commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(1u, commands.size());

  const auto& command = commands.front();
  for (const auto& regex : expected_regex) {
    EXPECT_TRUE(std::regex_search(command.RenderAllArgs(), regex.first))
        << "Regex " << regex.second << " failed";
  }
}

TEST(CommandTest, FillFlags) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c",            temp_input.c_str(),
                        "-o",      "/tmp/output.o", nullptr};
  const int argc = 5;

  List<Command> commands;
  ASSERT_TRUE(Command::GenerateFromArgs(argc, argv, commands));
  ASSERT_EQ(1u, commands.size());

  auto& command = commands.front();
  proto::Flags flags;
  command.FillFlags(&flags, "/some/clang/path");

  EXPECT_EQ(temp_input, flags.input());
  EXPECT_EQ("/tmp/output.o", flags.output());
  EXPECT_EQ("-emit-obj", flags.action());
  EXPECT_EQ("c++", flags.language());
  EXPECT_EQ("-cc1", *flags.other().begin());
  // TODO: add more expectations on flags.
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
        process->CallOnRun([this, process](ui32 timeout, const String& input,
                                           String* error) {
          // Client shouldn't run external processes.
          return false;
        });
      });
    }
  }

 protected:
  bool do_connect = true;
  net::ConnectionWeakPtr weak_ptr;
  ui32 send_count = 0, read_count = 0, connect_count = 0,
       connections_created = 0;
  Fn<void(net::TestConnection*)> connect_callback = EmptyLambda<>();
};

TEST_F(ClientTest, NoConnection) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  do_connect = false;
  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", "clang_path"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, NoEnvironmentVariable) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, String(), String()));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, NoInputFile) {
  const char* argv[] = {"clang++", "-c", "/tmp/qwerty", nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, CannotSendMessage) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->AbortOnSend();
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, CannotReadMessage) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

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
      EXPECT_EQ(temp_input, cc_flags.input());
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
        EXPECT_NE(end, std::find(begin, end, "-resource-dir"));
        EXPECT_NE(end, std::find(begin, end, "-internal-isystem"));
        EXPECT_NE(end, std::find(begin, end, "-internal-externc-isystem"));
      }

      {
        const auto& non_direct = cc_flags.non_direct();
        auto begin = non_direct.begin();
        auto end = non_direct.end();
        EXPECT_NE(end, std::find(begin, end, "-main-file-name"));
        EXPECT_NE(end, std::find(begin, end, "-coverage-file"));
      }
    });
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithoutStatus) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithBadStatus) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::INCONSEQUENT);
    });
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, SuccessfulCompilation) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::OK);
    });
  };

  EXPECT_FALSE(client::DoMain(argc, argv, String(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, FailedCompilation) {
  const String temp_input = base::CreateTempFile(".cc");
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::EXECUTION);
    });
  };

  EXPECT_EXIT(client::DoMain(argc, argv, String(), "clang++"),
              ::testing::ExitedWithCode(1), ".*");
}

TEST_F(ClientTest, DISABLED_MultipleCommands_OneFails) {
  // TODO: implement this test.
}

TEST_F(ClientTest, DISABLED_MultipleCommands_Successful) {
  // TODO: implement this test.
}

}  // namespace client
}  // namespace dist_clang
