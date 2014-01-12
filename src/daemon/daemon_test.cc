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
      listen_count = 0;

      factory = net::NetworkService::SetFactory<Service::Factory>();
    }

  protected:
    Daemon daemon;
    proto::Configuration config;
    Service::Factory* WEAK_PTR factory;

    uint listen_count;
};

TEST_F(DaemonTest, EmptyConfig) {
  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon.Initialize(configuration));
}

TEST_F(DaemonTest, InvalidConfig) {
  config.mutable_local()->set_port(1);

  daemon::Configuration configuration(config);

  ASSERT_FALSE(daemon.Initialize(configuration));
}

TEST_F(DaemonTest, ConfigWithSocketPath) {
  const std::string expected_socket_path = "/tmp/clangd.socket";

  config.set_socket_path(expected_socket_path);

  daemon::Configuration configuration(config);

  factory->CallOnCreate([&, this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([&, this](const std::string& host, uint16_t port) {
      EXPECT_EQ(expected_socket_path, host);
      EXPECT_EQ(0u, port);
    });
  });

  EXPECT_TRUE(daemon.Initialize(configuration));
  EXPECT_EQ(1u, listen_count);
}

TEST_F(DaemonTest, ConfigWithCachePath) {
  base::TemporaryDir tmp_dir;
  const std::string expected_cache_path = tmp_dir.GetPath() + "/cache";

  config.set_cache_path(expected_cache_path);

  daemon::Configuration configuration(config);

  EXPECT_FALSE(daemon.Initialize(configuration));
  EXPECT_FALSE(base::FileExists(expected_cache_path));
}

TEST_F(DaemonTest, ConfigWithLocal) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);

  daemon::Configuration configuration(config);

  factory->CallOnCreate([&, this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([&, this](const std::string& host, uint16_t port) {
      EXPECT_EQ(expected_host, host);
      EXPECT_EQ(expected_port, port);
    });
  });

  EXPECT_TRUE(daemon.Initialize(configuration));
  EXPECT_EQ(1u, listen_count);
}

TEST_F(DaemonTest, ConfigWithDisabledLocal) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_local()->set_host(expected_host);
  config.mutable_local()->set_port(expected_port);
  config.mutable_local()->set_disabled(true);

  daemon::Configuration configuration(config);

  factory->CallOnCreate([&, this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([&, this](const std::string& host, uint16_t port) {
      FAIL() << "Should not be called";
    });
  });

  EXPECT_FALSE(daemon.Initialize(configuration));
  EXPECT_EQ(0u, listen_count);
}

TEST_F(DaemonTest, ConfigWithStatistic) {
  const std::string expected_host = "localhost";
  const uint16_t expected_port = 7777;

  config.mutable_statistic()->set_host(expected_host);
  config.mutable_statistic()->set_port(expected_port);

  daemon::Configuration configuration(config);

  factory->CallOnCreate([&, this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([&, this](const std::string& host, uint16_t port) {
      EXPECT_EQ(expected_host, host);
      EXPECT_EQ(expected_port, port);
    });
  });

  EXPECT_FALSE(daemon.Initialize(configuration));
  EXPECT_EQ(1u, listen_count);
}

TEST_F(DaemonTest, ConfigWithDisabledStatistic) {
  config.mutable_statistic()->set_host("localhost");
  config.mutable_statistic()->set_port(7777);
  config.mutable_statistic()->set_disabled(true);

  daemon::Configuration configuration(config);

  factory->CallOnCreate([this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([this](const std::string& host, unsigned short port) {
      FAIL() << "Should not be called";
    });
  });

  EXPECT_FALSE(daemon.Initialize(configuration));
  EXPECT_EQ(0u, listen_count);
}

TEST_F(DaemonTest, BadLocalIncomingMessage) {
  config.set_socket_path("/tmp/clangd.socket");

  daemon::Configuration configuration(config);

  factory->CallOnCreate([this](Service* service) {
    service->CountListenAttempts(&listen_count);
    service->CallOnListen([this](const std::string& host, unsigned short port) {
      FAIL() << "Should not be called";
    });
  });
}

}  // namespace daemon
}  // namespace dist_clang
