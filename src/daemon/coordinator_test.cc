#include <daemon/coordinator.h>

#include <daemon/common_daemon_test.h>
#include <perf/stat_service.h>

namespace dist_clang {
namespace daemon {

class CoordinatorTest : public CommonDaemonTest {
 protected:
  UniquePtr<Coordinator> coordinator;
};

TEST_F(CoordinatorTest, ConfigurationRespond) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const ui16 number_of_remotes = 10;
  const ui32 total_shards = 5;
  const ui32 shard_queue_limit = 10;

  conf.mutable_coordinator()->mutable_local()->set_host(expected_host);
  conf.mutable_coordinator()->mutable_local()->set_port(expected_port);
  conf.mutable_coordinator()->set_total_shards(total_shards);
  conf.mutable_coordinator()->set_shard_queue_limit(shard_queue_limit);

  Vector<proto::Host> coordinated_remotes;
  coordinated_remotes.reserve(number_of_remotes);
  for (ui16 remote = 0; remote < number_of_remotes; ++remote) {
    proto::Host host;
    host.set_host(std::to_string(remote));
    host.set_port(expected_port + remote);
    host.set_threads(remote);
    host.set_ipv6(remote % 2 == 0);
    host.set_shard(remote);
    coordinated_remotes.push_back(host);

    // Also push it to coordinator's config.
    conf.mutable_coordinator()->add_remotes()->CopyFrom(host);
  }

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Configuration::extension));
      const auto& config =
          message.GetExtension(proto::Configuration::extension);

      ASSERT_EQ(number_of_remotes, config.emitter().remotes_size());
      for (ui16 remote = 0; remote < number_of_remotes; ++remote) {
        const proto::Host& expected_host = coordinated_remotes[remote];
        const proto::Host& received_host = config.emitter().remotes(remote);
        EXPECT_EQ(expected_host.host(), received_host.host());
        EXPECT_EQ(expected_host.port(), received_host.port());
        EXPECT_EQ(expected_host.threads(), received_host.threads());
        EXPECT_EQ(expected_host.ipv6(), received_host.ipv6());
        EXPECT_EQ(expected_host.shard(), received_host.shard());
      }
      EXPECT_EQ(total_shards, config.emitter().total_shards());
      EXPECT_EQ(shard_queue_limit, config.emitter().shard_queue_limit());
    });
    return true;
  };

  coordinator.reset(new Coordinator(conf));
  ASSERT_TRUE(coordinator->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    message->MutableExtension(proto::Configuration::extension);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    coordinator.reset();
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

}  // namespace daemon
}  // namespace dist_clang
