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

  bool do_run = true;
  std::unique_ptr<Daemon> daemon = std::unique_ptr<Daemon>{new Daemon};
  proto::Configuration config;
  Service* WEAK_PTR test_service = nullptr;
  ListenCallback listen_callback = EmptyLambda<bool>(true);
  std::function<void(net::TestConnection*)> connect_callback = EmptyLambda<>();
  std::function<void(base::TestProcess*)> run_callback = EmptyLambda<>();

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

  config.set_socket_path(expected_socket_path);
  config.add_versions()->set_version("1.0");
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

  config.set_socket_path(expected_socket_path);
  auto* version = config.add_versions();
  version->set_version("1.0");
  version->set_path("a");
  version->add_plugins()->set_name("test_plugin");
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

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version("1.0");
  version->set_path("a");
  auto* plugin = version->add_plugins();
  plugin->set_name("test_plugin");
  plugin->set_path("b");

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
    message->MutableExtension(proto::Execute::extension)->set_remote(false);
    message->MutableExtension(proto::Execute::extension)->set_current_dir("a");

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

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version("1.0");
  version->set_path("a");
  auto* plugin = version->add_plugins();
  plugin->set_name("test_plugin");
  plugin->set_path("b");

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
    extension->set_current_dir("a");
    extension->mutable_cc_flags()->mutable_compiler()->set_version("2.0");

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

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path("a");

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
    extension->set_current_dir("a");
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name("bad_plugin");

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
  const std::string compiler_version = "1.0";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path("a");
  auto* plugin = version->add_plugins();
  plugin->set_name("test_plugin");
  plugin->set_path("b");

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
    extension->set_current_dir("a");
    auto* compiler = extension->mutable_cc_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name("bad_plugin");

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
  const std::string compiler_version = "1.0";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path("a");

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
    extension->set_current_dir("a");
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
  const std::string compiler_version = "1.0";

  config.set_socket_path(socket_path);
  auto* version = config.add_versions();
  version->set_version(compiler_version);
  version->set_path("a");

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
    extension->set_current_dir("a");
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

}  // namespace daemon
}  // namespace dist_clang
