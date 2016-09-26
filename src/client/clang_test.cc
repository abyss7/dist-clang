#include <base/base.pb.h>
#include <base/file_utils.h>
#include <base/process_impl.h>
#include <base/test_process.h>
#include <client/clang.h>
#include <net/network_service_impl.h>
#include <net/test_connection.h>
#include <net/test_network_service.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace client {

class ClientTest : public ::testing::Test {
 public:
  using Service = net::TestNetworkService;

  virtual void SetUp() override {
    {
      auto factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        service->CountConnectAttempts(&connect_count);
        service->CallOnConnect([this](net::EndPointPtr, String* error) {
          if (!do_connect) {
            if (error) {
              error->assign("Test service rejects connection intentionally");
            }
            return net::TestConnectionPtr();
          }

          auto connection = net::TestConnectionPtr(new net::TestConnection);
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
        process->CallOnRun([this, process](ui32, const String&, String* error) {
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

  virtual void TearDown() override {
    net::NetworkService::SetFactory<Service::Factory>();
    base::Process::SetFactory<base::TestProcess::Factory>();
  }

 protected:
  bool do_connect = true;
  bool do_run = true;
  net::ConnectionWeakPtr weak_ptr;
  Atomic<ui32> send_count = {0}, read_count = {0}, connect_count = {0},
               connections_created = {0}, run_count = {0};
  Fn<void(net::TestConnection*)> connect_callback = EmptyLambda<>();
  Fn<void(base::TestProcess*)> run_callback = EmptyLambda<>();
};

namespace {

Literal clang_path = "fake_clang++"_l;
Literal version = "fake_version"_l;

}  // namespace

TEST_F(ClientTest, NoConnection) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  do_connect = false;
  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path"_l, clang_path, version,
                             0, 0, 0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, EmptyClangPath) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, Immutable(), Immutable(), Immutable(),
                             0, 0, 0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, Disabled) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path"_l, clang_path, version,
                             0, 0, 0, 0, HashMap<String, String>(), true));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, DISABLED_EmptyVersion) {
  // TODO: implement this test.
  //       - Check that we take version from clang itself.

  //  run_callback = [](base::TestProcess* process) {
  //    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
  //    process->stdout_ = "test_version\nline2\nline3"_l;
  //  };
}

TEST_F(ClientTest, NoInputFile) {
  const char* argv[] = {"clang++", "-c", "/tmp/qwerty", nullptr};
  const int argc = 3;

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path"_l, clang_path, version,
                             0, 0, 0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, CannotSendMessage) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->AbortOnSend();
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_TRUE(client::DoMain(argc, argv, String(), clang_path, version, 0, 0, 0,
                             0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, CannotReadMessage) {
  const String temp_input = "/tmp/test_file.cc";
  const char* argv[] = {"clang++", "-c", temp_input.c_str(), nullptr};
  const int argc = 3;

  connect_callback = [&](net::TestConnection* connection) {
    connection->AbortOnRead();
    connection->CallOnSend([&](const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(base::proto::Local::extension));

      const auto& extension =
          message.GetExtension(base::proto::Local::extension);
      EXPECT_EQ(base::GetCurrentDir(), Immutable(extension.current_dir()));
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
      }

      {
        const auto& non_cached = cc_flags.non_cached();
        auto begin = non_cached.begin();
        auto end = non_cached.end();
#if defined(OS_LINUX)
        EXPECT_NE(end, std::find(begin, end, "-internal-externc-isystem"));
        EXPECT_NE(end, std::find(begin, end, "-internal-isystem"));
#endif  // defined(OS_LINUX)
        EXPECT_NE(end, std::find(begin, end, "-resource-dir"));
      }

      {
        const auto& non_direct = cc_flags.non_direct();
        auto begin = non_direct.begin();
        auto end = non_direct.end();
        EXPECT_NE(end, std::find(begin, end, "-main-file-name"));
        EXPECT_NE(end, std::find(begin, end, "-coverage-notes-file"));
      }
    });
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_TRUE(client::DoMain(argc, argv, Immutable(), clang_path, version, 0, 0,
                             0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithoutStatus) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_TRUE(client::DoMain(argc, argv, Immutable(), clang_path, version, 0, 0,
                             0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithBadStatus) {
  const char* argv[] = {"clang++", "-c", "/tmp/test_file.cc", nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(net::proto::Status::extension);
      extension->set_code(net::proto::Status::INCONSEQUENT);
    });
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_TRUE(client::DoMain(argc, argv, Immutable(), clang_path, version, 0, 0,
                             0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, SuccessfulCompilation) {
  const char* argv[] = {"clang++", "-c", "/test_file.cc", nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(net::proto::Status::extension);
      extension->set_code(net::proto::Status::OK);
    });
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_FALSE(client::DoMain(argc, argv, Immutable(), clang_path, version, 0,
                              0, 0, 0, HashMap<String, String>(), false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, FailedCompilation) {
  const char* argv[] = {"clang++", "-c", "/test.cc", nullptr};
  const int argc = 3;

  connect_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(net::proto::Status::extension);
      extension->set_code(net::proto::Status::EXECUTION);
    });
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  EXPECT_EXIT(client::DoMain(argc, argv, String(), clang_path, version, 0, 0, 0,
                             0, HashMap<String, String>(), false),
              ::testing::ExitedWithCode(1), ".*");
}

TEST_F(ClientTest, DISABLED_MultipleCommands_OneFails) {
  // TODO: implement this test.
}

TEST_F(ClientTest, DISABLED_MultipleCommands_Successful) {
  // TODO: implement this test.
}

TEST_F(ClientTest, SendPluginPath) {
  const char* argv[] = {"clang++",     "-c",      "/test_file.cc", "-Xclang",
                        "-add-plugin", "-Xclang", "test-plugin",   nullptr};
  const int argc = 7;
  const String plugin_name = "test-plugin";
  const String plugin_path = "/test/plugin/path";

  connect_callback = [=](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(net::proto::Status::extension);
      extension->set_code(net::proto::Status::OK);
    });
    connection->CallOnSend([=](const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(base::proto::Local::extension));
      auto& extension = message.GetExtension(base::proto::Local::extension);
      ASSERT_TRUE(extension.has_flags());
      ASSERT_TRUE(extension.flags().has_compiler());
      ASSERT_EQ(1, extension.flags().compiler().plugins_size());
      EXPECT_EQ(plugin_name, extension.flags().compiler().plugins(0).name());
      EXPECT_EQ(plugin_path, extension.flags().compiler().plugins(0).path());
    });
  };
  run_callback = [](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"--version"_l}), process->args_);
    process->stdout_ = "test_version\nline2\nline3"_l;
  };

  HashMap<String, String> plugins;
  plugins.emplace(plugin_name, plugin_path);
  EXPECT_FALSE(client::DoMain(argc, argv, Immutable(), clang_path, version, 0,
                              0, 0, 0, plugins, false));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

}  // namespace client
}  // namespace dist_clang
