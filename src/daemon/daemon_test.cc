#include "daemon/daemon.h"

#include "base/file_utils.h"
#include "base/temporary_dir.h"
#include "gtest/gtest.h"
#include "net/network_service_impl.h"
#include "net/test_network_service.h"
#include "proto/config.pb.h"

namespace dist_clang {
namespace daemon {

class DaemonTest: public ::testing::Test {
  public:
    using Service = net::TestNetworkService<>;

    virtual void SetUp() override {
      listen_count = 0, send_count = 0, read_count = 0, connect_count = 0;
      connections_created = 0;
      test_service = nullptr;
      listen_callback = [](const std::string&, uint16_t) {};

      factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        ASSERT_EQ(nullptr, test_service);
        test_service = service;
        service->CountConnectAttempts(&connect_count);
        service->CountListenAttempts(&listen_count);
        service->CallOnConnect([this](net::TestConnection* connection) {
          connection->CountSendAttempts(&send_count);
          connection->CountReadAttempts(&read_count);
          ++connections_created;
        });
        service->CallOnListen(listen_callback);
      });
    }

  protected:
    Daemon daemon;
    proto::Configuration config;
    Service::Factory* WEAK_PTR factory;
    Service* WEAK_PTR test_service;
    std::function<void(const std::string&, uint16_t)> listen_callback;

    uint listen_count, connect_count, read_count, send_count;
    uint connections_created;
};

TEST_F(DaemonTest, EmptyConfig) {
  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon.Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, InvalidConfig) {
  config.mutable_local()->set_port(1);

  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon.Initialize(configuration));
  ASSERT_EQ(nullptr, test_service);
}

TEST_F(DaemonTest, ConfigWithSocketPath) {
  const std::string expected_socket_path = "/tmp/clangd.socket";

  config.set_socket_path(expected_socket_path);
  listen_callback = [&](const std::string& host, uint16_t port) {
    EXPECT_EQ(expected_socket_path, host);
    EXPECT_EQ(0u, port);
  };

  daemon::Configuration configuration(config);

  EXPECT_TRUE(daemon.Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithCachePath) {
  base::TemporaryDir tmp_dir;
  const std::string expected_cache_path = tmp_dir.GetPath() + "/cache";

  config.set_cache_path(expected_cache_path);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon.Initialize(configuration));
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
  listen_callback = [&](const std::string& host, uint16_t port) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
  };

  daemon::Configuration configuration(config);

  EXPECT_TRUE(daemon.Initialize(configuration));
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

  EXPECT_FALSE(daemon.Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithStatistic) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_statistic()->set_host(expected_host);
  config.mutable_statistic()->set_port(expected_port);
  listen_callback = [&](const std::string& host, uint16_t port) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
  };

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon.Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, ConfigWithDisabledStatistic) {
  config.mutable_statistic()->set_host("localhost");
  config.mutable_statistic()->set_port(7777);
  config.mutable_statistic()->set_disabled(true);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon.Initialize(configuration));
  ASSERT_NE(nullptr, test_service);
  EXPECT_EQ(0u, listen_count);
  EXPECT_EQ(0u, connect_count);
}

TEST_F(DaemonTest, LocalConnection) {
  const std::string socket_path = "/tmp/clangd.socket";

  config.set_socket_path(socket_path);
  listen_callback = [&](const std::string& host, uint16_t port) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
  };

  daemon::Configuration configuration(config);

  EXPECT_TRUE(daemon.Initialize(configuration));

  net::ConnectionWeakPtr connection = test_service->TriggerListen(socket_path);

  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(0u, send_count);
  EXPECT_TRUE(connection.expired())
      << "Daemon must not store references to the connection";
}

}  // namespace daemon
}  // namespace dist_clang
