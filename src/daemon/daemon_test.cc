#include "daemon/daemon.h"

#include "base/file_utils.h"
#include "base/process_impl.h"
#include "base/temporary_dir.h"
#include "base/test_process.h"
#include "gtest/gtest.h"
#include "net/network_service_impl.h"
#include "net/test_network_service.h"
#include "proto/config.pb.h"

namespace dist_clang {
namespace daemon {

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
        service->CallOnConnect([this](net::EndPointPtr, std::string*) {
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
        process->CallOnRun([this, process](
            unsigned timeout, const std::string& input, std::string* error) {
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
  using ListenCallback =
      std::function<bool(const std::string&, uint16_t, std::string*)>;
  using ConnectCallback = std::function<void(net::TestConnection*)>;
  using RunCallback = std::function<void(base::TestProcess*)>;

  bool do_run = true;
  std::unique_ptr<Daemon> daemon = std::unique_ptr<Daemon>{new Daemon};
  proto::Configuration config;
  Service* WEAK_PTR test_service = nullptr;
  ListenCallback listen_callback = EmptyLambda<bool>(true);
  ConnectCallback connect_callback = EmptyLambda<>();
  RunCallback run_callback = EmptyLambda<>();
  std::mutex send_mutex;
  std::condition_variable send_condition;

  uint listen_count = 0, connect_count = 0, read_count = 0, send_count = 0,
       run_count = 0;
  uint connections_created = 0;
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
  const std::string expected_socket_path = "/tmp/clangd.socket";

  config.set_socket_path(expected_socket_path);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
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
  const std::string expected_socket_path = "/tmp/clangd.socket";
  const std::string compiler_version = "fake_compiler_version";

  config.set_socket_path(expected_socket_path);
  config.add_versions()->set_version(compiler_version);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
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
  const std::string expected_socket_path = "/tmp/clangd.socket";
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string plugin_name = "test_plugin";

  config.set_socket_path(expected_socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  version->add_plugins()->set_name(plugin_name);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
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
  const std::string expected_cache_path = tmp_dir.GetPath() + "/cache";

  config.set_cache_path(expected_cache_path);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  EXPECT_FALSE(base::FileExists(expected_cache_path));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithLocal) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
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
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  config.mutable_local()->set_disabled(true);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithStatistic) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_statistic()->set_host(expected_host);
  config.mutable_statistic()->set_port(expected_port);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithDisabledStatistic) {
  config.mutable_statistic()->set_host("localhost");
  config.mutable_statistic()->set_port(7777);
  config.mutable_statistic()->set_disabled(true);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon->Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, LocalConnection) {
  const std::string socket_path = "/tmp/clangd.socket";

  config.set_socket_path(socket_path);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  net::ConnectionWeakPtr connection = test_service->TriggerListen(socket_path);

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(0u, send_count);
  EXPECT_TRUE(connection.expired())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, BadLocalMessage) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::BAD_MESSAGE;
  const std::string expected_description = "Test description";

  config.set_socket_path(socket_path);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
      EXPECT_EQ(expected_description, status.description());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    proto::Status status;
    status.set_code(expected_code);
    status.set_description(expected_description);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithoutCommand) {
  const std::string socket_path = "/tmp/clangd.socket";

  config.set_socket_path(socket_path);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_THROW_STD(
        test_connection->TriggerReadAsync(std::move(message), status),
        "Assertion failed: false");
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithoutCurrentDir) {
  const std::string socket_path = "/tmp/clangd.socket";

  config.set_socket_path(socket_path);
  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    message->MutableExtension(proto::Execute::extension)->set_remote(false);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_THROW_STD(
        test_connection->TriggerReadAsync(std::move(message), status),
        "Assertion failed: false");
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithNoCompiler) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string plugin_name = "test_plugin";
  const std::string plugin_path = "fake_plugin_path";
  const std::string current_dir = "fake_current_dir";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithBadCompiler) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
  const std::string compiler_version = "fake_compiler_version";
  const std::string bad_version = "another_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string plugin_name = "test_plugin";
  const std::string plugin_path = "fake_plugin_path";
  const std::string current_dir = "fake_current_dir";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);
    extension->mutable_cc_flags()->mutable_compiler()->set_version(bad_version);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithBadPlugin) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
  const std::string compiler_version = "1.0";
  const std::string compiler_path = "fake_compiler_path";
  const std::string current_dir = "fake_current_dir";
  const std::string bad_plugin_name = "bad_plugin_name";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalMessageWithBadPlugin2) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string plugin_name = "test_plugin";
  const std::string plugin_path = "fake_plugin_path";
  const std::string current_dir = "fake_current_dir";
  const std::string bad_plugin_name = "another_plugin_name";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalSuccessfulCompilation) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::OK;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string current_dir = "fake_current_dir";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, LocalFailedCompilation) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::EXECUTION;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string current_dir = "fake_current_dir";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  do_run = false;

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    daemon.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, StoreLocalCache) {
  const std::string socket_path = "/tmp/clangd.socket";
  const proto::Status::Code expected_code = proto::Status::OK;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string input_path1 = "test1.cc";
  const std::string input_path2 = "test2.cc";
  const std::string output_path1 = "test1.o";
  const std::string output_path2 = "test2.o";

  const base::TemporaryDir temp_dir;
  config.set_socket_path(socket_path);
  config.set_cache_path(temp_dir);
  config.set_sync_cache(true);

  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((std::list<std::string>{"-o", "-", input_path1}),
                process->args_);
    } else if (run_count == 2) {
      EXPECT_EQ((std::list<std::string>{"-o", output_path1, input_path1}),
                process->args_);
      EXPECT_TRUE(
          base::WriteFile(process->cwd_path_ + "/" + output_path1, "object"));
    } else if (run_count == 3) {
      EXPECT_EQ((std::list<std::string>{"-o", "-", input_path2}),
                process->args_);
    }
  };

  daemon::Configuration configuration(config);

  ASSERT_TRUE(daemon->Initialize(configuration));

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    base::TemporaryDir temp_dir;
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::Execute::extension);
    extension->set_remote(false);
    extension->set_current_dir(temp_dir);
    auto* compiler = extension->mutable_pp_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

    extension->mutable_cc_flags()->set_input(input_path1);
    extension->mutable_cc_flags()->set_output(output_path1);
    extension->mutable_pp_flags()->set_input(input_path1);

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
    auto* compiler = extension->mutable_pp_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

    extension->mutable_cc_flags()->set_input(input_path2);
    extension->mutable_cc_flags()->set_output(output_path2);
    extension->mutable_pp_flags()->set_input(input_path2);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    std::unique_lock<std::mutex> lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 2; });
  }

  daemon.reset();

  EXPECT_EQ(3u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(DaemonTest, StoreRemoteCache) {
  const base::TemporaryDir temp_dir;
  const std::string socket_path = "/tmp/clangd.socket";
  const std::string expected_host = "tmp_host";
  const uint16_t expected_port = 6002u;
  const proto::Status::Code expected_code = proto::Status::OK;
  const std::string compiler_version = "fake_compiler_version";
  const std::string compiler_path = "fake_compiler_path";
  const std::string input_path = "test.cc";
  const std::string output_path = "test.o";
  const std::string preprocessed_source = std::string();
  const std::string expected_object_code = "object_code";

  config.set_socket_path(socket_path);
  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  config.set_cache_path(temp_dir);
  config.set_sync_cache(true);

  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const std::string& host, uint16_t port, std::string*) {
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

    return true;
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
        EXPECT_EQ((std::list<std::string>{"-o", "-"}), process->args_);
        process->stdout_ = expected_object_code;
        break;
      }
      case 2: {
        EXPECT_EQ((std::list<std::string>{"-o", "-", input_path}),
                  process->args_);
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
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

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
    auto* compiler = extension->mutable_pp_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);

    extension->mutable_cc_flags()->set_input(input_path);
    extension->mutable_cc_flags()->set_output(output_path);
    extension->mutable_pp_flags()->set_input(input_path);

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
}

TEST_F(DaemonTest, DISABLED_DontStoreFailedRemoteCache) {
  // TODO: implement this.
}

}  // namespace daemon
}  // namespace dist_clang
