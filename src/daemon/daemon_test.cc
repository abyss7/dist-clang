#include <daemon/daemon.h>

#include <base/file_utils.h>
#include <base/process_impl.h>
#include <base/temporary_dir.h>
#include <base/test_process.h>
#include <daemon/configuration.pb.h>
#include <net/network_service_impl.h>
#include <net/test_network_service.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace daemon {

TEST(DaemonUtilTest, ConvertFlagsFromCC2PP) {
  proto::Flags expected_flags;
  auto* compiler = expected_flags.mutable_compiler();
  compiler->set_version("clang version 3.5");
  compiler->set_path("/usr/bin/clang");
  auto* plugin = compiler->add_plugins();
  plugin->set_name("some_plugin");
  plugin->set_path("/usr/lib/libplugin.so");
  expected_flags.add_non_cached()->assign("-I.");
  expected_flags.add_other()->assign("-cc1");
  expected_flags.set_action("-E");
  expected_flags.set_input("test.cc");
  expected_flags.set_output("-");
  expected_flags.set_language("c++");
  expected_flags.set_deps_file("some_deps_file");

  proto::Flags flags;
  flags.mutable_compiler()->set_version("clang version 3.5");
  flags.mutable_compiler()->set_path("/usr/bin/clang");
  plugin = flags.mutable_compiler()->add_plugins();
  plugin->set_name("some_plugin");
  plugin->set_path("/usr/lib/libplugin.so");
  flags.add_cc_only()->assign("-mrelax-all");
  flags.add_non_cached()->assign("-I.");
  flags.add_other()->assign("-cc1");
  flags.set_action("-emit-obj");
  flags.set_input("test.cc");
  flags.set_output("test.o");
  flags.set_language("c++");
  flags.set_deps_file("some_deps_file");

  proto::Flags actual_flags = Daemon::ConvertFlags(flags);
  EXPECT_EQ(expected_flags.SerializeAsString(),
            actual_flags.SerializeAsString());
}

class DaemonTest : public ::testing::Test {
 public:
  using Service = net::TestNetworkService;

  virtual void SetUp() override {
    {
      auto factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        ASSERT_EQ(nullptr, test_service);
        test_service = service;
        service->CountConnectAttempts(&connect_count);
        service->CountListenAttempts(&listen_count);
        service->CallOnConnect([this](net::EndPointPtr, String*) {
          auto connection = Service::TestConnectionPtr(new net::TestConnection);
          connection->CountSendAttempts(&send_count);
          connection->CountReadAttempts(&read_count);
          ++connections_created;
          connect_callback(connection.get());

          return connection;
        });
        service->CallOnListen(listen_callback);
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
  using ListenCallback = Fn<bool(const String&, ui16, String*)>;
  using ConnectCallback = Fn<void(net::TestConnection*)>;
  using RunCallback = Fn<void(base::TestProcess*)>;

  bool do_run = true;
  UniquePtr<Daemon> daemon = UniquePtr<Daemon>{new Daemon};
  proto::Configuration config;
  Service* WEAK_PTR test_service = nullptr;
  ListenCallback listen_callback = EmptyLambda<bool>(true);
  ConnectCallback connect_callback = EmptyLambda<>();
  RunCallback run_callback = EmptyLambda<>();
  std::mutex send_mutex;
  std::condition_variable send_condition;

  ui32 listen_count = 0, connect_count = 0, read_count = 0, send_count = 0,
       run_count = 0;
  ui32 connections_created = 0;
};

TEST_F(DaemonTest, EmptyConfig) {
  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, InvalidConfig) {
  config.mutable_local()->set_port(1);

  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon->Initialize(configuration));
  ASSERT_EQ(nullptr, test_service);
}

TEST_F(DaemonTest, ConfigWithSocketPath) {
  const String expected_socket_path = "/tmp/clangd.socket";

  config.set_socket_path(expected_socket_path);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  EXPECT_TRUE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithBadCompiler) {
  const String expected_socket_path = "/tmp/clangd.socket";
  const String compiler_version = "fake_compiler_version";

  config.set_socket_path(expected_socket_path);
  config.add_versions()->set_version(compiler_version);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithBadPlugin) {
  const String expected_socket_path = "/tmp/clangd.socket";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";

  config.set_socket_path(expected_socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  version->add_plugins()->set_name(plugin_name);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithCachePath) {
  base::TemporaryDir tmp_dir;
  const String expected_cache_path = tmp_dir.GetPath() + "/cache";

  config.mutable_cache()->set_path(expected_cache_path);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  EXPECT_FALSE(base::FileExists(expected_cache_path));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, DISABLED_ConfigWithDisabledCache) {
  // TODO: implement this test.
}

TEST_F(DaemonTest, ConfigWithLocal) {
  const String expected_host = "localhost";
  const ui16 expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };

  daemon::Configuration configuration(config);

  EXPECT_TRUE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithDisabledLocal) {
  const String expected_host = "localhost";
  const ui16 expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  config.mutable_local()->set_disabled(true);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, StoreRemoteCache) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/clangd.socket";
  const String expected_host = "tmp_host";
  const ui16 expected_port = 6002u;
  const proto::Status::Code expected_code = proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String input_path = "test.cc";
  const String output_path = "test.o";
  const String preprocessed_source = "fake_source";
  const String expected_object_code = "object_code";

  config.set_socket_path(socket_path);
  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  config.mutable_cache()->set_path(temp_dir);
  config.mutable_cache()->set_sync(true);

  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    switch (listen_count) {
      case 1: {
        EXPECT_EQ(expected_host, host);
        EXPECT_EQ(expected_port, port);
        break;
      }
      case 2: {
        EXPECT_EQ(socket_path, host);
        EXPECT_EQ(0u, port);
        break;
      }
      default:
        ADD_FAILURE() << "Unexpected listen on " << host << ":" << port;
    }

    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      switch (send_count) {
        case 1: {
          EXPECT_TRUE(message.HasExtension(proto::Status::extension));
          EXPECT_TRUE(message.HasExtension(proto::RemoteResult::extension));
          const auto& status = message.GetExtension(proto::Status::extension);
          EXPECT_EQ(expected_code, status.code());
          const auto& result =
              message.GetExtension(proto::RemoteResult::extension);
          EXPECT_EQ(expected_object_code, result.obj());
          break;
        }
        case 2: {
          EXPECT_TRUE(message.HasExtension(proto::Status::extension));
          const auto& status = message.GetExtension(proto::Status::extension);
          EXPECT_EQ(expected_code, status.code());
          break;
        }
        default:
          // TODO: print message contents.
          ADD_FAILURE() << "Sending unexpected message!";
      }

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    switch (run_count) {
      case 1: {
        EXPECT_EQ(
            (List<String>{"fake_action", "-x", "fake_language", "-o", "-"}),
            process->args_);
        process->stdout_ = expected_object_code;
        break;
      }
      case 2: {
        EXPECT_EQ(
            (List<String>{"-E", "-x", "fake_language", "-o", "-", input_path}),
            process->args_);
        process->stdout_ = preprocessed_source;
        break;
      }
      default:
        ADD_FAILURE() << "Unexpected run of process! Args: " << process->args_;
    }
  };

  daemon::Configuration configuration(config);
  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection1 = test_service->TriggerListen(expected_host, expected_port);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(true);
    extension->set_pp_source(preprocessed_source);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");
    extension->mutable_flags()->set_language("fake_language");

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    std::unique_lock<std::mutex> lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 1; });
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    base::TemporaryDir temp_dir;
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");
    extension->mutable_flags()->set_language("fake_language");

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    std::unique_lock<std::mutex> lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 2; });
  }

  daemon.reset();

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(2u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(DaemonTest, DISABLED_DontStoreFailedRemoteCache) {
  // TODO: implement this test.
}

}  // namespace daemon
}  // namespace dist_clang
