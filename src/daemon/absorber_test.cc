#include <daemon/absorber.h>

#include <base/temporary_dir.h>
#include <daemon/common_daemon_test.h>

namespace dist_clang {
namespace daemon {

TEST(AbsorberConfigurationTest, NoAbsorberSection) {
  ASSERT_ANY_THROW((Absorber((proto::Configuration()))));
}

TEST(AbsorberConfigurationTest, NoLocalHost) {
  proto::Configuration conf;
  conf.mutable_absorber();

  ASSERT_ANY_THROW(Absorber absorber(conf));
}

TEST(AbsorberConfigurationTest, DISABLED_IgnoreDirectCache) {
  // TODO: implement this test. Check that "cache.direct" is really ignored.
}

class AbsorberTest : public CommonDaemonTest {
 protected:
  UniquePtr<Absorber> absorber;
};

TEST_F(AbsorberTest, SuccessfulCompilation) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const proto::Status::Code expected_code = proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_TRUE(message.HasExtension(proto::RemoteResult::extension));
      EXPECT_TRUE(
          message.GetExtension(proto::RemoteResult::extension).has_obj());
    });
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension =
        message->MutableExtension(proto::RemoteExecute::extension);
    extension->set_source("fake_source");
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    absorber.reset();
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

TEST_F(AbsorberTest, FailedCompilation) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const proto::Status::Code expected_code = proto::Status::EXECUTION;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_FALSE(message.HasExtension(proto::RemoteResult::extension));
    });
  };
  do_run = false;

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension =
        message->MutableExtension(proto::RemoteExecute::extension);
    extension->set_source("fake_source");
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    absorber.reset();
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

TEST_F(AbsorberTest, StoreLocalCache) {
  const base::TemporaryDir temp_dir;
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const proto::Status::Code expected_code = proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const String source = "fake_source";
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      EXPECT_TRUE(message.HasExtension(proto::RemoteResult::extension));
      const auto& ext = message.GetExtension(proto::RemoteResult::extension);
      EXPECT_TRUE(ext.has_obj());
      EXPECT_EQ(String(object_code), ext.obj());

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{action, "-x"_l, language, "-o"_l, "-"_l}),
              process->args_);
    process->stdout_ = object_code;
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection1 = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension =
        message->MutableExtension(proto::RemoteExecute::extension);

    extension->set_source(source);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension =
        message->MutableExtension(proto::RemoteExecute::extension);

    extension->set_source(source);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  absorber.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check with deps file.
}

TEST_F(AbsorberTest, DISABLED_SkipTaskWithClosedConnection) {
  // TODO: implement this test.
  //       - Tasks should be skipped before any execution or sending attempt.
}

}  // namespace daemon
}  // namespace dist_clang
