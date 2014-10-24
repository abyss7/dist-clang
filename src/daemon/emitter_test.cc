#include <daemon/emitter.h>

#include <base/file_utils.h>
#include <base/process_impl.h>
#include <base/temporary_dir.h>
#include <base/test_process.h>
#include <net/test_network_service.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

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

class EmitterTest : public ::testing::Test {
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
        process->CallOnRun([this, process](ui32, const String&, String* error) {
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

  ListenCallback listen_callback = EmptyLambda<bool>(true);
  ConnectCallback connect_callback = EmptyLambda<>();
  RunCallback run_callback = EmptyLambda<>();

  UniquePtr<Emitter> emitter;
  Service* WEAK_PTR test_service = nullptr;
  proto::Configuration conf;
  bool do_run = true;

  // for test StoreLocalCache.
  std::mutex send_mutex;
  std::condition_variable send_condition;

  ui32 listen_count = 0, connect_count = 0, read_count = 0, send_count = 0,
       run_count = 0;
  ui32 connections_created = 0;
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
  const proto::Status::Code expected_code = proto::Status::BAD_MESSAGE;
  const String expected_description = "Test description";

  conf.mutable_emitter()->set_socket_path(socket_path);
  listen_callback = [&](const String& host, ui16 port, String*) {
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

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

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

TEST_F(EmitterTest, LocalMessageWithBadCompiler) {
  const String socket_path = "/tmp/test.socket";
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
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
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

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
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
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
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

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
  const proto::Status::Code expected_code = proto::Status::NO_VERSION;
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
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

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

TEST_F(EmitterTest, LocalSuccessfulCompilation) {
  const String socket_path = "/tmp/test.socket";
  const proto::Status::Code expected_code = proto::Status::OK;
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
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

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

TEST_F(EmitterTest, LocalFailedCompilation) {
  const String socket_path = "/tmp/test.socket";
  const proto::Status::Code expected_code = proto::Status::EXECUTION;
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
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
  };
  do_run = false;

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    proto::Status status;
    status.set_code(proto::Status::OK);

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
TEST_F(EmitterTest, StoreCacheForLocalResult) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/test.socket";
  const proto::Status::Code expected_code = proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String object_code = "fake_object_code";
  const String source = "fake_source";
  const String language = "fake_language";
  const String action = "fake_action";
  const String input_path1 = "test1.cc";
  const String input_path2 = "test2.cc";
  const String output_path1 = "test1.o";

  const String output_path2 = String(temp_dir) + "/test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_sync(true);
  conf.mutable_cache()->set_direct(false);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(proto::Status::extension));
      const auto& status = message.GetExtension(proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((List<String>{"-E", "-x", language, "-o", "-", input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((List<String>{action, "-x", language, "-o", output_path1,
                              input_path1}),
                process->args_);
      EXPECT_TRUE(base::WriteFile(process->cwd_path_ + "/" + output_path1,
                                  object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((List<String>{"-E", "-x", language, "-o", "-", input_path2}),
                process->args_);
      process->stdout_ = source;
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path1);
    extension->mutable_flags()->set_output(output_path1);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 1; });
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path2);
    extension->mutable_flags()->set_output(output_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 2; });
  }

  emitter.reset();

  String cache_output;
  EXPECT_TRUE(base::FileExists(output_path2));
  EXPECT_TRUE(base::ReadFile(output_path2, &cache_output));
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

  // TODO: check with deps file.
  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

/*
TEST_F(EmitterTest, StoreCacheForRemoteResult) {
  const base::TemporaryDir temp_dir;
  const String socket_path = "/tmp/test.socket";
  const String host = "fake_host";
  const ui16 port = 12345;
  const proto::Status::Code expected_code = proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String object_code = "fake_object_code";
  const String source = "fake_source";
  const String language = "fake_language";
  const String action = "fake_action";
  const String input_path1 = "test1.cc";
  const String input_path2 = "test2.cc";
  const String output_path1 = "test1.o";

  const String output_path2 = String(temp_dir) + "/test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  conf.mutable_emitter()->set_socket_path(socket_path);
  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_sync(true);
  conf.mutable_cache()->set_direct(false);

  auto* remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(host);
  remote->set_port(port);
  remote->set_threads(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  listen_callback = [&](const String& host, ui16 port, String*) {
    EXPECT_EQ(socket_path, host);
    EXPECT_EQ(0u, port);
    return !::testing::Test::HasNonfatalFailure();
  };

  connect_callback = [&](net::TestConnection* connection) {
    if (connect_count == 1) {
      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(proto::Status::extension));
        const auto& status = message.GetExtension(proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(proto::RemoteExecute::extension));
        const auto& command =
            message.GetExtension(proto::RemoteExecute::extension);
        EXPECT_EQ(source, command.source());
        LOG(ERROR) << command.flags().compiler().version();
      });
    }
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((List<String>{"-E", "-x", language, "-o", "-", input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((List<String>{"-E", "-x", language, "-o", "-", input_path2}),
                process->args_);
      process->stdout_ = source;
    }
  };

  emitter.reset(new Emitter(conf));
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path1);
    extension->mutable_flags()->set_output(output_path1);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 1; });
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    std::shared_ptr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(proto::LocalExecute::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path2);
    extension->mutable_flags()->set_output(output_path2);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    proto::Status status;
    status.set_code(proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    send_condition.wait_for(lock, std::chrono::seconds(1),
                            [this] { return send_count == 2; });
  }

  emitter.reset();

  String cache_output;
  EXPECT_TRUE(base::FileExists(output_path2));
  EXPECT_TRUE(base::ReadFile(output_path2, &cache_output));
  EXPECT_EQ(object_code, cache_output);

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
  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}
*/

TEST_F(EmitterTest, DISABLED_StoreDirectCacheForLocalResult) {
  // TODO: implement this test. Check that we store direct cache for successful
  //       local compilation.
}

TEST_F(EmitterTest, DISABLED_StoreDirectCacheForRemoteResult) {
  // TODO: implement this test. Check that we store direct cache for successful
  //       remote compilation.
}

TEST_F(EmitterTest, DISABLED_UpdateDirectCacheFromLocalCache) {
  // TODO: implement this test.
  //       - Check that direct cache gets updated, if there is direct cache
  //         miss, but there is hit in normal cache.
}

}  // namespace daemon
}  // namespace dist_clang
