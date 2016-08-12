#include <daemon/emitter.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/temporary_dir.h>
#include <daemon/common_daemon_test.h>
#include <net/test_connection.h>

namespace dist_clang {
namespace daemon {

TEST(EmitterConfigurationTest, NoEmitterSection) {
  ASSERT_ANY_THROW((Emitter((proto::Configuration()))));
}

TEST(EmitterConfigurationTest, DISABLED_ZeroThreads) {
  proto::Configuration conf;
  conf.mutable_emitter()->set_threads(0);

  // FIXME: this will cause a hang, since the |~QueueAggregator| tries to delete
  //        un-joined threads.
  ASSERT_ANY_THROW((Emitter(conf)));
}

TEST(EmitterConfigurationTest, NoSocketPath) {
  proto::Configuration conf;
  conf.mutable_emitter();

  ASSERT_ANY_THROW(Emitter emitter(conf));
}

TEST(EmitterConfigurationTest, OnlyFailedWithoutRemotes) {
  proto::Configuration conf;
  conf.mutable_emitter()->set_socket_path("/tmp/test.socket");
  conf.mutable_emitter()->set_only_failed(true);

  Emitter emitter(conf);
  ASSERT_FALSE(emitter.Initialize());
}

class EmitterTest : public CommonDaemonTest {
 protected:
  UniquePtr<Emitter> emitter;
};

/*
 * We can connect, and when we connect the emitter waits for message.
 */
TEST_F(EmitterTest, IdleConnection) {
  const String socket_path = "/tmp/test.socket";

  conf.mutable_emitter()->set_socket_path(socket_path);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  net::ConnectionWeakPtr connection = test_service->TriggerListen(socket_path);

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count) << "Daemon must try to read a sole message";
  EXPECT_EQ(0u, send_count);
  EXPECT_TRUE(connection.expired())
      << "Daemon must not store references to the connection";
}

/*
 * If we receive a bad message from client, then say it back to him.
 */
TEST_F(EmitterTest, BadLocalMessage) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::BAD_MESSAGE;
  const String expected_description = "Test description";

  conf.mutable_emitter()->set_socket_path(socket_path);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
      EXPECT_EQ(expected_description, status.description());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    net::proto::Status status;
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

TEST_F(EmitterTest, LocalMessageWithoutCommand) {
  const String socket_path = "/tmp/test.socket";

  conf.mutable_emitter()->set_socket_path(socket_path);
  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

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

/*
 * If a client requests compiler version, that we don't have - send NO_VERSION.
 * This test checks |DoLocalExecute()|.
 */
TEST_F(EmitterTest, LocalMessageWithBadCompiler) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
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

/*
 * If a client requests compiler version, that we don't have - send NO_VERSION.
 * This test checks |DoRemoteExecute()|.
 */
TEST_F(EmitterTest, RemoteMessageWithBadCompiler) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";
  const String host = "fake_host";
  const ui16 port = 12345;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_emitter()->set_only_failed(true);

  auto* remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(host);
  remote->set_port(port);
  remote->set_threads(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
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

TEST_F(EmitterTest, LocalMessageWithBadPlugin) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "1.0";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";
  const String bad_plugin_name = "bad_plugin_name";

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
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

TEST_F(EmitterTest, LocalMessageWithBadPlugin2) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";
  const String bad_plugin_name = "another_plugin_name";

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
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

TEST_F(EmitterTest, LocalMessageWithPluginPath) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    auto* plugin = compiler->add_plugins();
    plugin->set_name(plugin_name);
    plugin->set_path(plugin_path);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, LocalMessageWithSanitizeBlacklist) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const auto action = "fake_action"_l;
  const auto sanitize_blacklist_path = "asan-blacklist.txt"_l;
  const ui32 user_id = 1234;

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-fsanitize-blacklist"_l,
                               sanitize_blacklist_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_sanitize_blacklist(sanitize_blacklist_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, ConfigurationWithoutVersions) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  conf.mutable_emitter()->set_socket_path(socket_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->set_path(compiler_path);
    auto* plugin = compiler->add_plugins();
    plugin->set_name(plugin_name);
    plugin->set_path(plugin_path);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, LocalSuccessfulCompilation) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
  }

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, DISABLED_RemoteSuccessfulCompilation) {
  // TODO: implement this test.
  //       - Check the permissions of object and deps files, if the client
  //         provides the user_id.
  //       - Check the outgoing message doesn't have compiler and plugins paths.
}

TEST_F(EmitterTest, LocalFailedCompilation) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::EXECUTION;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  do_run = false;

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
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

/*
 * 1. Store new entry in the cache after a local compilation.
 * 2. Try to compile the same preprocessed source locally.
 * 3. Restore saved entry from the cache without compilation.
 */
TEST_F(EmitterTest, StoreSimpleCacheForLocalResult) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;
  const auto input_path1 = "test1.cc"_l;
  const auto input_path2 = "test2.cc"_l;
  const auto output_path1 = "test1.o"_l;
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;

  const String output_path2 = String(temp_dir) + "/test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l,
                                 language, "-o"_l, output_path1, input_path1}),
                process->args_);
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + output_path1,
                                    object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path2}),
                process->args_);
      process->stdout_ = source;
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path1);
    extension->mutable_flags()->set_output(output_path1);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path2);
    extension->mutable_flags()->set_output(output_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  emitter.reset();

  Immutable cache_output;
  EXPECT_TRUE(base::File::Exists(output_path2));
  EXPECT_TRUE(base::File::Read(output_path2, &cache_output));
  EXPECT_EQ(object_code, cache_output);

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

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, StoreSimpleCacheForRemoteResult) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/test.socket";
  const String host = "fake_host";
  const ui16 port = 12345;
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const String action = "fake_action";
  const auto input_path1 = "test1.cc"_l;
  const auto input_path2 = "test2.cc"_l;
  const String output_path1 = "test1.o";
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;

  const String output_path2 = String(temp_dir) + "/test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(host);
  remote->set_port(port);
  remote->set_threads(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    if (connect_count == 1) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
        const auto& status =
            message.GetExtension(net::proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(proto::Remote::extension));
        const auto& command = message.GetExtension(proto::Remote::extension);
        EXPECT_EQ(source, command.source());

        send_condition.notify_all();
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Result::extension)
            ->set_obj(object_code);
      });
    } else if (connect_count == 3) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
        const auto& status =
            message.GetExtension(net::proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    }
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path2}),
                process->args_);
      process->stdout_ = source;
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);

    extension->set_current_dir(temp_dir);
    extension->mutable_flags()->set_input(input_path1);
    extension->mutable_flags()->set_output(output_path1);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
    // FIXME: describe, why |send_count == 2| ?
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path2);
    extension->mutable_flags()->set_output(output_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 3; }));
  }

  emitter.reset();

  Immutable cache_output;
  ASSERT_TRUE(base::File::Exists(output_path2));
  ASSERT_TRUE(base::File::Read(output_path2, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(3u, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(3u, read_count);
  EXPECT_EQ(3u, send_count)
      << "There should be only these transmissions:" << std::endl
      << "  1. Local daemon -> remote daemon." << std::endl
      << "  2. Local daemon -> 1st client." << std::endl
      << "  3. Local daemon -> 2nd client.";
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, StoreSimpleCacheForLocalResultWithAndWithoutBlacklist) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/test.socket";
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;
  const auto input_path = "test.cc"_l;
  const auto output_path1 = "test1.o"_l;
  const auto output_path2 = "test2.o"_l;
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;
  const auto sanitize_blacklist_path = "asan-blacklist.txt"_l;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l,
                                 language, "-o"_l, output_path1, input_path}),
                process->args_);
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 4) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l, language,
              "-fsanitize-blacklist"_l, sanitize_blacklist_path, "-o"_l, output_path2, input_path}),
        process->args_);
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path1);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);
    extension->mutable_flags()->set_sanitize_blacklist(sanitize_blacklist_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  emitter.reset();

  EXPECT_EQ(4u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, StoreDirectCacheForLocalResult) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto path = Immutable(String(temp_dir));
  const auto input1_path = path + "/test1.cc"_l;
  const auto input2_path = path + "/test2.cc"_l;
  const auto header1_path = path + "/header1.h"_l;
  const auto header2_path = path + "/header2.h"_l;
  const auto source_code = "int main() {}"_l;

  ASSERT_TRUE(base::File::Write(input1_path, source_code));
  ASSERT_TRUE(base::File::Write(input2_path, source_code));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  // Prepare configuration.
  const String socket_path = "/tmp/test.socket";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  // Prepare callbacks.
  const auto expected_code = net::proto::Status::OK;
  const auto deps1_path = "test1.d"_l;
  const auto deps2_path = path + "test2.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output1_path = "test1.o"_l;
  const auto object_code = "fake_object_code"_l;
  const auto deps_contents = "test1.o: test1.cc header1.h header2.h"_l;

  const auto output2_path = path + "/test2.o"_l;
  // |output_path2| checks that everything works fine with absolute paths.

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps1_path,
                                 "-x"_l, language, "-o"_l, "-"_l, input1_path}),
                process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + deps1_path,
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps1_path, "-x"_l,
                                 language, "-o"_l, output1_path, input1_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + output1_path,
                                    object_code));
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input1_path);
    extension->mutable_flags()->set_output(output1_path);
    extension->mutable_flags()->set_deps_file(deps1_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input2_path);
    extension->mutable_flags()->set_output(output2_path);
    extension->mutable_flags()->set_deps_file(deps2_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  emitter.reset();

  Immutable cache_output;
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

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

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

TEST_F(EmitterTest, StoreDirectCacheForRemoteResult) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto path = Immutable(String(temp_dir));
  const auto input1_path = path + "/test1.cc"_l;
  const auto input2_path = path + "/test2.cc"_l;
  const auto header1_path = path + "/header1.h"_l;
  const auto header2_path = path + "/header2.h"_l;
  const auto source_code = "int main() {}"_l;

  ASSERT_TRUE(base::File::Write(input1_path, source_code));
  ASSERT_TRUE(base::File::Write(input2_path, source_code));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

  // Prepare configuration.
  const String socket_path = "/tmp/test.socket";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const String host = "fake_host";
  const ui16 port = 12345;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(host);
  remote->set_port(port);
  remote->set_threads(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  // Prepare callbacks.
  const auto expected_code = net::proto::Status::OK;
  const auto deps1_path = "test1.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output1_path = "test1.o"_l;
  const auto object_code = "fake_object_code"_l;

  const auto output2_path = path + "/test2.o"_l;
  // |output_path2| checks that everything works fine with absolute paths.

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    if (connect_count == 1) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
        const auto& status =
            message.GetExtension(net::proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(proto::Remote::extension));
        const auto& command = message.GetExtension(proto::Remote::extension);
        EXPECT_EQ(preprocessed_source, command.source());

        send_condition.notify_all();
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Result::extension)
            ->set_obj(object_code);
      });
    } else if (connect_count == 3) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
        const auto& status =
            message.GetExtension(net::proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    }
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps1_path, "-x"_l,
                               language, "-o"_l, "-"_l, input1_path}),
              process->args_);
    process->stdout_ = preprocessed_source;
    EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + deps1_path,
                                  "test1.o: test1.cc header1.h header2.h"_l));
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input1_path);
    extension->mutable_flags()->set_output(output1_path);
    extension->mutable_flags()->set_deps_file(deps1_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input2_path);
    extension->mutable_flags()->set_output(output2_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 3; }));
  }

  emitter.reset();

  Immutable cache_output;
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(3u, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(3u, read_count);
  EXPECT_EQ(3u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, DISABLED_UpdateDirectCacheFromLocalCache) {
  // TODO: implement this test.
  //       - Check that direct cache gets updated, if there is direct cache
  //         miss, but there is hit in normal cache.
}

TEST_F(EmitterTest, DISABLED_SkipTaskWithClosedConnection) {
  // TODO: implement this test.
  //       - Tasks should be skipped before any execution or sending attempt.
  //       - Tasks should be skipped during the cache check, local and remote
  //         executions.
}

/*
 * If |conf_| has no version requested by connection,
 * |connection| should return |message| with bad status,
 * but if then |conf_| has been updated with a missing version,
 * |connection| should return |message| with ok status.
 */
TEST_F(EmitterTest, UpdateConfiguration) {
  const String socket_path = "/tmp/test.socket";
  const auto expected_code_no_version = net::proto::Status::NO_VERSION;
  const auto expected_code_ok = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String plugin_name = "test_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const String current_dir = "fake_current_dir";
  const auto action = "fake_action"_l;

  conf.mutable_emitter()->set_socket_path(socket_path);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return true;
  };
  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code_no_version, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  version = conf.add_versions();
  version->set_version(bad_version);
  version->set_path(compiler_path);

  // TODO: change sleep_for on sync event
  std::this_thread::sleep_for(std::chrono::seconds(1));
  emitter->UpdateConfiguration(conf);

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code_ok, status.code());
    });
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action}), process->args_)
        << process->PrintArgs();
  };

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    emitter.reset();
  }

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
}

TEST_F(EmitterTest, HitDirectCacheFromTwoLocations) {
  // Prepare environment.
  const base::TemporaryDir temp_dir1, temp_dir2;
  const auto path = Immutable(String(temp_dir1));
  const auto input_path = path + "/test.cc"_l;
  const auto header_path = path + "/header.h"_l;
  const auto sanitize_blacklist_path = path + "/asan-blacklist.txt"_l;
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto sanitize_blacklist_contents = "fun:main"_l;

  ASSERT_TRUE(base::File::Write(input_path, source_code));
  ASSERT_TRUE(base::File::Write(header_path, header_contents));
  ASSERT_TRUE(
      base::File::Write(sanitize_blacklist_path, sanitize_blacklist_contents));

  // Prepare configuration.
  const String socket_path = "/tmp/test.socket";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir1);
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  // Prepare callbacks.
  const auto expected_code = net::proto::Status::OK;
  const auto deps_path = "test.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output_path = "test.o"_l;
  const auto object_code = "fake_object_code"_l;
  const auto deps_contents = "test.o: test.cc header.h"_l;

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ(
          (Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path, "-x"_l,
                           language, "-o"_l, "-"_l, input_path}),
          process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + deps_path,
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{
                    action, "-load"_l, plugin_path, "-dependency-file"_l,
                    deps_path, "-x"_l, language, "-fsanitize-blacklist"_l,
                    sanitize_blacklist_path, "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + output_path,
                                    object_code));
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir1);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->set_deps_file(deps_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);
    extension->mutable_flags()->set_sanitize_blacklist(sanitize_blacklist_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    const auto path = Immutable(String(temp_dir2));
    const auto input_path = path + "/test.cc"_l;
    const auto header_path = path + "/header.h"_l;
    const auto sanitize_blacklist_path = path + "/asan-blacklist.txt"_l;

    ASSERT_TRUE(base::File::Write(input_path, source_code));
    ASSERT_TRUE(base::File::Write(header_path, header_contents));
    ASSERT_TRUE(base::File::Write(sanitize_blacklist_path,
                                  sanitize_blacklist_contents));

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir2);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->set_deps_file(deps_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);
    extension->mutable_flags()->set_sanitize_blacklist(sanitize_blacklist_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  emitter.reset();

  Immutable cache_output;
  const auto output2_path = String(temp_dir2) + "/" + String(output_path);
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = String(temp_dir2) + "/" + String(deps_path);
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

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

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

TEST_F(EmitterTest, DontHitDirectCacheFromTwoRelativeSources) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto path = Immutable(String(temp_dir));
  const auto relpath = path + "/path1"_l;
  ASSERT_TRUE(base::CreateDirectory(relpath));

  const auto input_path = relpath + "/test.cc"_l;
  const auto header_path = relpath + "/header.h"_l;
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto header_contents2 = "#define B"_l;

  const auto relpath2 = path + "/path2"_l;
  ASSERT_TRUE(base::CreateDirectory(relpath2));

  const auto input_path2 = relpath2 + "/test.cc"_l;
  const auto header_path2 = relpath2 + "/header.h"_l;

  ASSERT_TRUE(base::File::Write(input_path, source_code));
  ASSERT_TRUE(base::File::Write(header_path, header_contents));

  ASSERT_TRUE(base::File::Write(input_path2, source_code));
  ASSERT_TRUE(base::File::Write(header_path2, header_contents2));

  // Prepare configuration.
  const String socket_path = "/tmp/test.socket";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  // Prepare callbacks.
  const auto expected_code = net::proto::Status::OK;

  const auto deps_path = "path1/test.d"_l;
  const auto deps_path2 = "path2/test.d"_l;

  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto preprocessed_source2 = "fake_source2"_l;
  const auto action = "fake_action"_l;

  const auto output_path = "path1/test.o"_l;
  const auto output_path2 = "path2/test.o"_l;

  const auto object_code = "fake_object_code1"_l;
  const auto object_code2 = "fake_object_code2"_l;

  const auto deps_contents = "path1/test.o: path1/test.cc path1/header.h"_l;
  const auto deps_contents2 = "path2/test.o: path2/test.cc path2/header.h"_l;

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path,
                                 "-x"_l, language, "-o"_l, "-"_l, input_path}),
                process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + deps_path,
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps_path, "-x"_l,
                                 language, "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + output_path,
                                    object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path2,
                                 "-x"_l, language, "-o"_l, "-"_l, input_path2}),
                process->args_);
      process->stdout_ = preprocessed_source2;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + deps_path2,
                                    deps_contents2));
    } else if (run_count == 4) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps_path2, "-x"_l,
                                 language, "-o"_l, output_path2, input_path2}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ + "/"_l + output_path2,
                                    object_code2));
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->set_deps_file(deps_path);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path2);
    extension->mutable_flags()->set_output(output_path2);
    extension->mutable_flags()->set_deps_file(deps_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1),
                                        [this] { return send_count == 2; }));
  }

  emitter.reset();

  Immutable cache_output;
  const auto output2_path = String(temp_dir) + "/" + String(output_path);
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = String(temp_dir) + "/" + String(deps_path);
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

  Immutable cache_output2;
  const auto output2_path2 = String(temp_dir) + "/" + String(output_path2);
  EXPECT_TRUE(base::File::Exists(output2_path2));
  EXPECT_TRUE(base::File::Read(output2_path2, &cache_output2));
  EXPECT_EQ(object_code2, cache_output2);

  Immutable cache_deps2;
  const auto deps2_path2 = String(temp_dir) + "/" + String(deps_path2);
  EXPECT_TRUE(base::File::Exists(deps2_path2));
  EXPECT_TRUE(base::File::Read(deps2_path2, &cache_deps2));
  EXPECT_EQ(deps_contents2, cache_deps2);

  EXPECT_EQ(4u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

}  // namespace daemon
}  // namespace dist_clang
