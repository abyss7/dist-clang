#include <daemon/collector.h>

#include <daemon/common_daemon_test.h>
#include <perf/stat_service.h>

namespace dist_clang {
namespace daemon {

class CollectorTest : public CommonDaemonTest {
 protected:
  UniquePtr<Collector> collector;
};

TEST_F(CollectorTest, SimpleReport) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;

  STAT(DIRECT_CACHE_HIT);
  STAT(SIMPLE_CACHE_HIT);
  STAT(SIMPLE_CACHE_HIT);
  STAT(SIMPLE_CACHE_HIT);

  conf.mutable_collector()->mutable_local()->set_host(expected_host);
  conf.mutable_collector()->mutable_local()->set_port(expected_port);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(perf::proto::Report::extension));
      const auto& report = message.GetExtension(perf::proto::Report::extension);
      EXPECT_EQ(2, report.metric_size());
      EXPECT_EQ(perf::proto::Metric::DIRECT_CACHE_HIT, report.metric(0).name());
      EXPECT_EQ(1u, report.metric(0).value());
      EXPECT_EQ(perf::proto::Metric::SIMPLE_CACHE_HIT, report.metric(1).name());
      EXPECT_EQ(3u, report.metric(1).value());
    });
  };

  collector.reset(new Collector(conf));
  ASSERT_TRUE(collector->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(perf::proto::Report::extension);
    extension->add_metric()->set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
    extension->add_metric()->set_name(perf::proto::Metric::SIMPLE_CACHE_HIT);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    collector.reset();
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
