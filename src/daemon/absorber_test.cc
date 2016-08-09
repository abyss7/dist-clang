#include <daemon/absorber.h>

#include <base/temporary_dir.h>
#include <daemon/common_daemon_test.h>

namespace dist_clang {
namespace daemon {

namespace {

// S1..5 types can be one of the following: |String|, |Immutable|, |Literal|
template <typename S1, typename S2, typename S3, typename S4 = String,
          typename S5 = String>
net::Connection::ScopedMessage CreateMessage(
    const S1& source, const S2& action, const S3& compiler_version,
    const S4& language = S4(), const S5& sanitize_blacklist = S5()) {
  net::Connection::ScopedMessage message(new net::Connection::Message);
  auto* extension = message->MutableExtension(proto::Remote::extension);
  extension->set_source(source);
  auto* compiler = extension->mutable_flags()->mutable_compiler();
  compiler->set_version(compiler_version);
  extension->mutable_flags()->set_action(action);
  if (language != String()) {
    extension->mutable_flags()->set_language(language);
  }
  if (sanitize_blacklist != String()) {
    extension->set_sanitize_blacklist(sanitize_blacklist);
  }

  return message;
}

net::proto::Status StatusFor(net::proto::Status::Code status_code) {
  net::proto::Status status;
  status.set_code(status_code);
  return status;
}

net::proto::Status StatusOK() {
  return StatusFor(net::proto::Status::OK);
}

}  // namespace

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
  const net::proto::Status::Code expected_code = net::proto::Status::OK;
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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_TRUE(message.HasExtension(proto::Result::extension));
      EXPECT_TRUE(message.GetExtension(proto::Result::extension).has_obj());
    });
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(
        CreateMessage("fake_source"_l, "fake_action"_l, compiler_version));

    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));
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

TEST_F(AbsorberTest, SuccessfulCompilationWithBlacklist) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const net::proto::Status::Code expected_code = net::proto::Status::OK;
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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_TRUE(message.HasExtension(proto::Result::extension));
      EXPECT_TRUE(message.GetExtension(proto::Result::extension).has_obj());
    });
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage("fake_source"_l, "fake_action"_l,
                               compiler_version, "", "sanitize_blacklist"));

    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));
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
  const net::proto::Status::Code expected_code = net::proto::Status::EXECUTION;
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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_FALSE(message.HasExtension(proto::Result::extension));
    });
  };
  do_run = false;

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(
        CreateMessage("fake_source"_l, "fake_action"_l, compiler_version));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));
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

TEST_F(AbsorberTest, StoreLocalCacheWithoutBlacklist) {
  const base::TemporaryDir temp_dir;
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const net::proto::Status::Code expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const String source = "fake_source";
  const auto language = "c++"_l, converted_language = "c++-cpp-output"_l;
  const auto action = "fake_action"_l;

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      EXPECT_TRUE(message.HasExtension(proto::Result::extension));
      const auto& ext = message.GetExtension(proto::Result::extension);
      EXPECT_TRUE(ext.has_obj());
      EXPECT_EQ(String(object_code), ext.obj());

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(
        (Immutable::Rope{action, "-x"_l, converted_language, "-o"_l, "-"_l}),
        process->args_);
    process->stdout_ = object_code;
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection1 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

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

TEST_F(AbsorberTest, StoreLocalCacheWithBlacklist) {
  const base::TemporaryDir temp_dir;
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const net::proto::Status::Code expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const String source = "fake_source";
  const auto language = "c++"_l, converted_language = "c++-cpp-output"_l;
  const auto action = "fake_action"_l;
  const String sanitize_blacklist = "fake_sanitize_blacklist";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      EXPECT_TRUE(message.HasExtension(proto::Result::extension));
      const auto& ext = message.GetExtension(proto::Result::extension);
      EXPECT_TRUE(ext.has_obj());
      EXPECT_EQ(String(object_code), ext.obj());

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    auto process_args_iter = process->args_.begin();
    EXPECT_EQ(action, *(process_args_iter++));
    EXPECT_EQ("-x"_l, *(process_args_iter++));
    EXPECT_EQ(converted_language, *(process_args_iter++));
    EXPECT_EQ("-fsanitize-blacklist"_l, *(process_args_iter++));
    ++process_args_iter;  // we don't know in advance which filename will
                          // sanitize blacklist have on absorber
    EXPECT_EQ("-o"_l, *(process_args_iter++));
    EXPECT_EQ("-"_l, *(process_args_iter++));
    EXPECT_EQ(process->args_.end(), process_args_iter);
    process->stdout_ = object_code;
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection1 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language,
                               sanitize_blacklist));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language,
                               sanitize_blacklist));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

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

TEST_F(AbsorberTest, StoreLocalCacheWithAndWithoutBlacklist) {
  const base::TemporaryDir temp_dir;
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const net::proto::Status::Code expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const String source = "fake_source";
  const auto language = "c++"_l;
  const auto action = "fake_action"_l;
  const String sanitize_blacklist = "fake_sanitize_blacklist";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

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
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      EXPECT_TRUE(message.HasExtension(proto::Result::extension));
      const auto& ext = message.GetExtension(proto::Result::extension);
      EXPECT_TRUE(ext.has_obj());
      EXPECT_EQ(String(object_code), ext.obj());

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    process->stdout_ = object_code;
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection1 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(CreateMessage(source, action, compiler_version, language,
                               sanitize_blacklist));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  absorber.reset();

  EXPECT_EQ(2u, run_count);
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

TEST_F(AbsorberTest, BadCompilerVersion) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const net::proto::Status::Code expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "real_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String bad_compiler_version = "bad_compiler_version";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&expected_host, expected_port](const String& host,
                                                    ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [expected_code](net::TestConnection* connection) {
    connection->CallOnSend([expected_code](
        const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_FALSE(message.HasExtension(proto::Result::extension));
    });
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    auto message(
        CreateMessage("fake_source"_l, "fake_action"_l, bad_compiler_version));
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    EXPECT_TRUE(
        test_connection->TriggerReadAsync(std::move(message), StatusOK()));
    absorber.reset();
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

TEST_F(AbsorberTest, BadMessageStatus) {
  const String expected_host = "fake_host";
  const ui16 expected_port = 12345;
  const auto expected_code = net::proto::Status::BAD_MESSAGE;
  const String compiler_version = "compiler_version";
  const String compiler_path = "fake_compiler_path";

  conf.mutable_absorber()->mutable_local()->set_host(expected_host);
  conf.mutable_absorber()->mutable_local()->set_port(expected_port);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&expected_host, expected_port](const String& host,
                                                    ui16 port, String*) {
    EXPECT_EQ(expected_host, host);
    EXPECT_EQ(expected_port, port);
    return true;
  };
  connect_callback = [expected_code](net::TestConnection* connection) {
    connection->CallOnSend([expected_code](
        const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());

      EXPECT_FALSE(message.HasExtension(proto::Result::extension));
    });
  };

  absorber.reset(new Absorber(conf));
  ASSERT_TRUE(absorber->Initialize());

  auto connection = test_service->TriggerListen(expected_host, expected_port);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    EXPECT_TRUE(test_connection->TriggerReadAsync(
        CreateMessage(""_l, ""_l, ""_l), StatusFor(expected_code)));
    absorber.reset();
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
