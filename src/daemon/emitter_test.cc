#include <daemon/emitter.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/temporary_dir.h>
#include <daemon/common_daemon_test.h>
#include <net/test_connection.h>
#include <perf/stat_service.h>

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
  EmitterTest() : socket_path("/tmp/test.socket") {
    auto* emitter = conf.mutable_emitter();
    emitter->set_socket_path(socket_path);

    listen_callback = [this](const String& host, ui16 port, String*) {
      EXPECT_EQ(socket_path, host);
      EXPECT_EQ(0u, port);
      return !::testing::Test::HasNonfatalFailure();
    };
  }

  UniquePtr<Emitter> emitter;
  const String socket_path;
};

/*
 * We can connect, and when we connect the emitter waits for message.
 */
TEST_F(EmitterTest, IdleConnection) {
  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::BAD_MESSAGE;
  const String expected_description = "Test description";

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
      EXPECT_EQ(expected_description, status.description());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    auto message = std::make_unique<net::Connection::Message>();
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
  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
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

namespace {

// FIXME: maybe make it a lambda?
String EndPointString(const String& host, const ui16 port = 0) {
  return host + ":" + std::to_string(port);
}

}  // namespace

TEST_F(EmitterTest, ConfigurationUpdateFromCoordinator) {
  const base::TemporaryDir temp_dir;
  const auto expected_code = net::proto::Status::OK;
  const auto action = "fake_action"_l;
  const auto handled_source1 = "fake_source1"_l;
  const auto handled_source2 = "fake_source2"_l;
  const String object_code = "fake_object_code";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String output_path = "test.o";
  const String default_remote_host = "default_remote_host";
  const ui16 default_remote_port = 1;
  const String old_remote_host = "old_remote_host";
  const ui16 old_remote_port = 2;
  const String new_remote_host = "new_remote_host";
  const ui16 new_remote_port = 3;
  const String coordinator_host = "coordinator_host";
  const ui16 coordinator_port = 4;
  const ui32 old_total_shards = 5;
  const ui32 new_total_shards = 6;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_emitter()->set_poll_interval(1u);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(coordinator_host);
  coordinator->set_port(coordinator_port);

  auto* remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(default_remote_host);
  remote->set_port(default_remote_port);
  remote->set_threads(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    // All connection callbacks are on emitter side.

    if (connect_count == 1) {
      // Connection from emitter to coordinator.

      EXPECT_EQ(EndPointString(coordinator_host, coordinator_port),
                end_point->Print());

      connection->CallOnRead([&](net::Connection::Message* message) {
        auto* emitter =
            message->MutableExtension(proto::Configuration::extension)
                ->mutable_emitter();
        auto* remote = emitter->add_remotes();
        remote->set_host(old_remote_host);
        remote->set_port(old_remote_port);
        remote->set_shard(1);
        remote->set_threads(1);

        emitter->set_total_shards(old_total_shards);
      });
    } else if (connect_count == 2) {
      // Connection from emitter to coordinator.

      EXPECT_EQ(EndPointString(coordinator_host, coordinator_port),
                end_point->Print());

      connection->CallOnSend([this](const net::Connection::Message&) {
        send_condition.notify_all();
        // Run the task #1 on old remote before sending anything to coordinator.

        UniqueLock lock(send_mutex);
        EXPECT_TRUE(send_condition.wait_for(
            lock, Seconds(1), [this] { return send_count == 3; }));
        // Send #1: emitter → coordinator.
        // Send #2: emitter → coordinator (current one).
        // Send #3: emitter → remote.
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        auto* emitter =
            message->MutableExtension(proto::Configuration::extension)
                ->mutable_emitter();
        auto* remote = emitter->add_remotes();
        remote->set_host(new_remote_host);
        remote->set_port(new_remote_port);
        remote->set_shard(1);
        remote->set_threads(1);

        emitter->set_total_shards(new_total_shards);
      });
    } else if (connect_count == 3) {
      // Connection from local client to emitter. Task #1.

      EXPECT_EQ(EndPointString(socket_path, 0), end_point->Print());

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
        const auto& status =
            message.GetExtension(net::proto::Status::extension);
        EXPECT_EQ(expected_code, status.code()) << status.description();
      });
    } else if (connect_count == 4) {
      // Connection from emitter to remote absorber. Task #1.

      EXPECT_EQ(EndPointString(old_remote_host, old_remote_port),
                end_point->Print());
      EXPECT_EQ(old_total_shards, emitter->conf()->emitter().total_shards());

      connection->CallOnSend([this](const net::Connection::Message&) {
        send_condition.notify_all();
        // Continue sending message to coordinator.
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Result::extension)
            ->set_obj(object_code);
      });
    } else if (connect_count == 5) {
      // Connection from emitter to coordinator.

      EXPECT_EQ(EndPointString(coordinator_host, coordinator_port),
                end_point->Print());

      connection->CallOnSend([this](const net::Connection::Message&) {
        send_condition.notify_all();
        // Run the task #2 on new remote before sending anything to coordinator.
      });
    } else if (connect_count == 6) {
      // Connection from local client to emitter. Task #2.

      EXPECT_EQ(EndPointString(socket_path, 0), end_point->Print());
    } else if (connect_count == 7) {
      // Connection from emitter to remote absorber. Task #2.

      EXPECT_EQ(EndPointString(new_remote_host, new_remote_port),
                end_point->Print());
      DCHECK(emitter);
      EXPECT_EQ(new_total_shards, emitter->conf()->emitter().total_shards());

      connection->CallOnSend([this](const net::Connection::Message&) {
        send_condition.notify_all();
        // Now we can reset |emitter|.
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Result::extension)
            ->set_obj(object_code);
      });
    }

    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"-E"_l, "-o"_l, "-"_l}), process->args_);
    if (run_count == 1) {
      process->stdout_ = handled_source1;
    } else if (run_count == 2) {
      process->stdout_ = handled_source2;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  {
    // Wait for a second connection to coordinator:
    //  * 1st second - wait for queue to pop in default remote.
    //  * 2nd second - wait for coordinator to poll.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(3),
                                        [this] { return send_count == 2; }));
    // Send #1: emitter → coordinator.
    // Send #2: emitter → coordinator (not complete at this moment).
  }

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    auto test_connection =
        std::static_pointer_cast<net::TestConnection>(connection1);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    // Wait for a third connection to coordinator:
    //  * 1st second - wait for queue to pop in old remote.
    //  * 2nd second - wait for coordinator to poll.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(3),
                                        [this] { return send_count == 5; }));
    // Send #3: emitter → remote.
    // Send #4: emitter → local.
    // Send #5: emitter → coordinator (not complete at this moment).
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    auto test_connection =
        std::static_pointer_cast<net::TestConnection>(connection2);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    // Wait for a connection to remote absorber.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1),
                                        [this] { return send_count == 6; }));
    // Send #6: emitter → remote.
    // Send #7: emitter → local.
  }

  emitter.reset();

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(7u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";
}

/*
 * Check that emitter doesn't enter infinite loop while polling coordinators.
 */
TEST_F(EmitterTest, NoGoodCoordinator) {
  const String bad_coordinator_host1 = "bad_host1";
  const ui16 bad_coordinator_port1 = 1;
  const String bad_coordinator_host2 = "bad_host2";
  const ui16 bad_coordinator_port2 = 2;

  conf.mutable_emitter()->set_only_failed(false);
  conf.mutable_emitter()->set_poll_interval(1u);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host1);
  coordinator->set_port(bad_coordinator_port1);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host2);
  coordinator->set_port(bad_coordinator_port2);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host1, bad_coordinator_port1));
      return false;
    }
    if (connections_created == 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host2, bad_coordinator_port2));
      return false;
    }
    if (connections_created > 2) {
      send_condition.notify_all();
      return false;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(2), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, send_count);
}

/*
 * Coordinator should get rotated if emitter can't connect to it:
 *   - connect to bad coordinator
 *   - (rotate coordinators)
 *   - connect to good coordinator without waiting
 *   - (wait poll interval)
 *   - connect to good coordinator again
 */
TEST_F(EmitterTest, CoordinatorNoConnection) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;

  conf.mutable_emitter()->set_only_failed(false);
  conf.mutable_emitter()->set_poll_interval(1u);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      return false;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Configuration::extension);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(2), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created - 1, read_count);
  EXPECT_EQ(connections_created - 1, send_count);
}

/*
 * Coordinator should get rotated if emitter can't send to it:
 *   - connect to bad coordinator
 *   - try to send message
 *   - (rotate coordinators)
 *   - (wait poll interval)
 *   - connect to good coordinator without waiting
 *   - (wait poll interval)
 *   - connect to good coordinator again
 */
TEST_F(EmitterTest, CoordinatorSendFailed) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;

  conf.mutable_emitter()->set_only_failed(false);
  conf.mutable_emitter()->set_poll_interval(1u);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      connection->AbortOnSend();
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Configuration::extension);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(3), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created - 1, read_count);
  EXPECT_EQ(connections_created, send_count);
}

/*
 * Coordinator should get rotated if emitter can't read from it:
 *   - connect to bad coordinator
 *   - try to read message
 *   - (rotate coordinators)
 *   - (wait poll interval)
 *   - connect to good coordinator without waiting
 *   - (wait poll interval)
 *   - connect to good coordinator again
 */
TEST_F(EmitterTest, CoordinatorReadFailed) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;

  conf.mutable_emitter()->set_only_failed(false);
  conf.mutable_emitter()->set_poll_interval(1u);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      connection->AbortOnRead();
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Configuration::extension);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(3), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
}

/*
 * Coordinator should get rotated if it doesn't provide configuration.
 */
TEST_F(EmitterTest, CoordinatorNoConfiguration) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;

  conf.mutable_emitter()->set_only_failed(false);
  conf.mutable_emitter()->set_poll_interval(1u);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {});
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Configuration::extension);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(3), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
}

/*
 * Coordinator should get rotated if it doesn't provide remotes
 * when |only_failed| is true.
 */
TEST_F(EmitterTest, CoordinatorNoRemotes) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;
  const String remote_host = "fake_host";
  const ui16 remote_port = 12345;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_emitter()->set_poll_interval(1u);

  auto remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(remote_host);
  remote->set_port(remote_port);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->MutableExtension(proto::Configuration::extension);
      });
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->MutableExtension(proto::Configuration::extension)
                          ->mutable_emitter()
                          ->add_remotes();
        remote->set_host(remote_host);
        remote->set_port(remote_port);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(5), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
}

/*
 * Coordinator should get rotated if it doesn't provide enabled remotes
 * when |only_failed| is true.
 */
TEST_F(EmitterTest, CoordinatorNoEnabledRemotes) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;
  const String remote_host = "fake_host";
  const ui16 remote_port = 12345;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_emitter()->set_poll_interval(1u);

  auto remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(remote_host);
  remote->set_port(remote_port);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->MutableExtension(proto::Configuration::extension)
                          ->mutable_emitter()
                          ->add_remotes();
        remote->set_host(remote_host);
        remote->set_port(remote_port);
        remote->set_disabled(true);
      });
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(),
                EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->MutableExtension(proto::Configuration::extension)
                          ->mutable_emitter()
                          ->add_remotes();
        remote->set_host(remote_host);
        remote->set_port(remote_port);
      });
      if (connections_created > 2) {
        send_condition.notify_all();
      }
      return true;
    }

    NOTREACHED();
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(5), [this] { return connections_created > 2; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
}

TEST_F(EmitterTest, CoordinatorSuccessfulUpdate) {
  const String bad_coordinator_host = "bad_host";
  const ui16 bad_coordinator_port = 1;
  const String good_coordinator_host = "good_host";
  const ui16 good_coordinator_port = 2;
  const String remote_host = "fake_host";
  const ui16 remote_port = 12345;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_emitter()->set_poll_interval(1u);

  auto remote = conf.mutable_emitter()->add_remotes();
  remote->set_host(remote_host);
  remote->set_port(remote_port);

  // FIXME(ilezhankin): in this test we rely on order of coordinators inside
  //                    a Protobuf object.
  auto coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(good_coordinator_host);
  coordinator->set_port(good_coordinator_port);

  coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(bad_coordinator_host);
  coordinator->set_port(bad_coordinator_port);

  connect_callback = [&](net::TestConnection* connection,
                         net::EndPointPtr end_point) {
    EXPECT_EQ(end_point->Print(),
              EndPointString(good_coordinator_host, good_coordinator_port));
    connection->CallOnRead([&](net::Connection::Message* message) {
      auto remote = message->MutableExtension(proto::Configuration::extension)
                        ->mutable_emitter()
                        ->add_remotes();
      remote->set_host(remote_host);
      remote->set_port(remote_port);
    });

    if (connections_created > 1) {
      send_condition.notify_all();
    }

    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(
      lock, Seconds(4), [this] { return connections_created > 1; }));

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(2u, connections_created);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
}

/*
 * If a client requests compiler version, that we don't have - send NO_VERSION.
 * This test checks |DoRemoteExecute()|.
 */
TEST_F(EmitterTest, RemoteMessageWithBadCompiler) {
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";
  const String host = "fake_host";
  const ui16 port = 12345;

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "1.0";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";
  const String bad_plugin_name = "bad_plugin_name";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::NO_VERSION;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "test_plugin";
  const String plugin_path = "fake_plugin_path";
  const String current_dir = "fake_current_dir";
  const String bad_plugin_name = "another_plugin_name";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const auto action = "fake_action"_l;
  const auto sanitize_blacklist_path = "asan-blacklist.txt"_l;
  const ui32 user_id = 1234;

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, Immutable("-fsanitize-blacklist="_l) +
                                           Immutable(sanitize_blacklist_path)}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(compiler_path, process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto expected_code = net::proto::Status::EXECUTION;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code());
    });
    return true;
  };
  do_run = false;

  emitter = std::make_unique<Emitter>(conf);
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

  const Path output_path2 = temp_dir.GetPath() / "test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
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
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1.c_str(),
                                    object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path2}),
                process->args_);
      process->stdout_ = source;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::SIMPLE_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

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
  const auto remote_compilation_time = std::chrono::milliseconds(10);

  const Path output_path2 = temp_dir.GetPath() / "test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
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
        // Used to check remote compilation timing statistics.
        std::this_thread::sleep_for(remote_compilation_time);
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
    return true;
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

  emitter = std::make_unique<Emitter>(conf);
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

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::SIMPLE_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  perf::proto::Metric remote_compilation_time_metric;
  remote_compilation_time_metric.set_name(
      perf::proto::Metric::REMOTE_COMPILATION_TIME);
  base::Singleton<perf::StatService>::Get().Dump(
      remote_compilation_time_metric);
  EXPECT_GE(remote_compilation_time_metric.value(),
            static_cast<ui64>(remote_compilation_time.count()));

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

/*
 * 1. Trigger compilation on remote
 * 2. Remote fails compilation
 * 3. Run local compilation.
 */
TEST_F(EmitterTest, FallbackToLocalCompilationAfterRemoteFail) {
  const base::TemporaryDir temp_dir;
  const String host = "fake_host";
  const ui16 port = 12345;
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;
  const auto input_path1 = "test1.cc"_l;
  const auto output_path1 = "test1.o"_l;
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;
  const auto local_compilation_time = std::chrono::milliseconds(10);

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
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
        message->MutableExtension(net::proto::Status::extension)
            ->set_code(net::proto::Status::EXECUTION);
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      // Used to check local compilation timing statistics.
      std::this_thread::sleep_for(local_compilation_time);
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l,
                                 language, "-o"_l, output_path1, input_path1}),
                process->args_);
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1.c_str(),
                                    object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

  emitter.reset();

  perf::proto::Metric remote_compilation_failed_metric;
  remote_compilation_failed_metric.set_name(
      perf::proto::Metric::REMOTE_COMPILATION_FAILED);
  base::Singleton<perf::StatService>::Get().Dump(
      remote_compilation_failed_metric);
  EXPECT_EQ(1u, remote_compilation_failed_metric.value());

  perf::proto::Metric local_compilation_time_metric;
  local_compilation_time_metric.set_name(
      perf::proto::Metric::LOCAL_COMPILATION_TIME);
  base::Singleton<perf::StatService>::Get().Dump(local_compilation_time_metric);
  EXPECT_GE(local_compilation_time_metric.value(),
            static_cast<ui64>(local_compilation_time.count()));

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count)
      << "There should be only two transmissions:" << std::endl
      << "  1. From client -> local daemon." << std::endl
      << "  2. Local daemon -> remote daemon." << std::endl;
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
}

/*
 * 1. Trigger compilation on remote
 * 2. Remote rejects compilation
 * 3. Run local compilation.
 */
TEST_F(EmitterTest, FallbackToLocalCompilationAfterRemoteRejects) {
  const base::TemporaryDir temp_dir;
  const String host = "fake_host";
  const ui16 port = 12345;
  const auto expected_code = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;
  const auto input_path1 = "test1.cc"_l;
  const auto output_path1 = "test1.o"_l;
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;
  const auto preprocess_time = std::chrono::milliseconds(10);

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
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
        message->MutableExtension(net::proto::Status::extension)
            ->set_code(net::proto::Status::OVERLOAD);
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      // Used to check preprocess timing statistics.
      std::this_thread::sleep_for(preprocess_time);
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l,
                                 input_path1}),
                process->args_);
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l,
                                 language, "-o"_l, output_path1, input_path1}),
                process->args_);
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1.c_str(),
                                    object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

  emitter.reset();

  perf::proto::Metric remote_compilation_failed_metric;
  remote_compilation_failed_metric.set_name(
      perf::proto::Metric::REMOTE_COMPILATION_FAILED);
  base::Singleton<perf::StatService>::Get().Dump(
      remote_compilation_failed_metric);
  EXPECT_EQ(0u, remote_compilation_failed_metric.value());

  perf::proto::Metric remote_compilation_rejected_metric;
  remote_compilation_rejected_metric.set_name(
      perf::proto::Metric::REMOTE_COMPILATION_REJECTED);
  base::Singleton<perf::StatService>::Get().Dump(
      remote_compilation_rejected_metric);
  EXPECT_EQ(1u, remote_compilation_rejected_metric.value());

  perf::proto::Metric preprocess_time_metric;
  preprocess_time_metric.set_name(perf::proto::Metric::PREPROCESS_TIME);
  base::Singleton<perf::StatService>::Get().Dump(preprocess_time_metric);
  EXPECT_GE(preprocess_time_metric.value(),
            static_cast<ui64>(preprocess_time.count()));

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count)
      << "There should be only these transmissions:" << std::endl
      << "  1. Local daemon -> remote daemon." << std::endl
      << "  2. Local daemon -> client." << std::endl;
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, StoreSimpleCacheForLocalResultWithAndWithoutBlacklist) {
  const base::TemporaryDir temp_dir;
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

  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
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
      EXPECT_EQ(
          (Immutable::Rope{action, "-load"_l, plugin_path, "-x"_l, language,
                           Immutable("-fsanitize-blacklist="_l) +
                               Immutable(sanitize_blacklist_path),
                           "-o"_l, output_path2, input_path}),
          process->args_);
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

  ASSERT_TRUE(base::File::Write(input1_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(input2_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header1_path.string_copy(), "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path.string_copy(), "#define B"_l));

  // Prepare configuration.
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps1_path,
                                 "-x"_l, language, "-o"_l, "-"_l, input1_path}),
                process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps1_path.c_str(),
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps1_path, "-x"_l,
                                 language, "-o"_l, output1_path, input1_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output1_path.c_str(),
                                    object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  Immutable cache_output;
  EXPECT_TRUE(base::File::Exists(output2_path.string_copy()));
  EXPECT_TRUE(base::File::Read(output2_path.string_copy(), &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  EXPECT_TRUE(base::File::Exists(deps2_path.string_copy()));
  EXPECT_TRUE(base::File::Read(deps2_path.string_copy(), &cache_deps));
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

TEST_F(EmitterTest,
       StoreDirectCacheForLocalResultWithAndWithoutIncludedHeaders) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto path = Immutable(String(temp_dir));
  const auto input_path = path + "/test.cc"_l;
  const auto header1_path = path + "/header1.h"_l;
  const auto header2_path = path + "/header2.h"_l;
  const auto source_code = "int main() {}"_l;
  const auto expected_code = net::proto::Status::OK;
  const auto deps_path = "test.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output_path = "test.o"_l;
  const auto object_code = "fake_object_code"_l;
  const auto preprocessed_header_path = path + "/header1.h.pth"_l;
  const auto deps_contents = "test.o: test.cc header1.h header2.h"_l;

  // Clang outputs headers included using "-include-pth/pch" as absolute paths.
  const auto preprocessed_deps_contents =
      Immutable("test.o: test.cc header1.h header2.h "_l) + header1_path;
  const auto preprocessed_contents = "Any content should work"_l;

  ASSERT_TRUE( base::File::Write(preprocessed_header_path.string_copy(),
                                 preprocessed_contents));

  ASSERT_TRUE(base::File::Write(input_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header1_path.string_copy(), "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path.string_copy(), "#define B"_l));

  // Prepare configuration.
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

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
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path,
                                 "-x"_l, language, "-o"_l, "-"_l, input_path}),
                process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path.c_str(),
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps_path, "-x"_l,
                                 language, "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path.c_str(),
                                    object_code));
    } else if (run_count == 3) {
      EXPECT_EQ(
          (Immutable::Rope{"-E"_l, "-include-pth"_l, preprocessed_header_path,
                           "-dependency-file"_l, deps_path, "-x"_l, language,
                           "-o"_l, "-"_l, input_path}),
          process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path.c_str(),
                                    preprocessed_deps_contents));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);
    extension->mutable_flags()->add_non_cached("-include-pth");
    extension->mutable_flags()->add_non_cached(preprocessed_header_path);
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
                                        [this] { return send_count == 2; }));
  }

  auto connection3 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection3);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);
    extension->mutable_flags()->add_non_cached("-include-pth");
    extension->mutable_flags()->add_non_cached(preprocessed_header_path);
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
                                        [this] { return send_count == 3; }));
  }

  emitter.reset();

  EXPECT_EQ(3u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(3u, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(3u, read_count);
  EXPECT_EQ(3u, send_count);
  EXPECT_EQ(1, connection1.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count())
      << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection3.use_count())
      << "Daemon must not store references to the connection";
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

  ASSERT_TRUE(base::File::Write(input1_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(input2_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header1_path.string_copy(), "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path.string_copy(), "#define B"_l));

  // Prepare configuration.
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const String host = "fake_host";
  const ui16 port = 12345;

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
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
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps1_path, "-x"_l,
                               language, "-o"_l, "-"_l, input1_path}),
              process->args_);
    process->stdout_ = preprocessed_source;
    EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps1_path.c_str(),
                                  "test1.o: test1.cc header1.h header2.h"_l));
  };

  emitter = std::make_unique<Emitter>(conf);
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

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  Immutable cache_output;
  EXPECT_TRUE(base::File::Exists(output2_path.string_copy()));
  EXPECT_TRUE(base::File::Read(output2_path.string_copy(), &cache_output));
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
TEST_F(EmitterTest, ConfigurationUpdateCompiler) {
  const auto expected_code_no_version = net::proto::Status::NO_VERSION;
  const auto expected_code_ok = net::proto::Status::OK;
  const String compiler_version = "fake_compiler_version";
  const String bad_version = "another_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String plugin_name = "test_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const String current_dir = "fake_current_dir";
  const auto action = "fake_action"_l;

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);
  auto* plugin = version->add_plugins();
  plugin->set_name(plugin_name);
  plugin->set_path(plugin_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code_no_version, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
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

  // TODO: replace |sleep_for()| with some sync event.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  emitter->Update(conf);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code_ok, status.code());
    });
    return true;
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
  const auto preprocessed_header_path = path + "header.h.pth"_l;
  const auto sanitize_blacklist_path = path + "/asan-blacklist.txt"_l;
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto preprocessed_contents = "Any content should work"_l;
  const auto sanitize_blacklist_contents = "fun:main"_l;

  ASSERT_TRUE(base::File::Write(input_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header_path.string_copy(), header_contents));
  ASSERT_TRUE(base::File::Write(preprocessed_header_path.string_copy(),
                                preprocessed_contents));
  ASSERT_TRUE(base::File::Write(sanitize_blacklist_path.string_copy(),
                                sanitize_blacklist_contents));

  // Prepare configuration.
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

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

  // Clang outputs headers included using "-include-pth/pch" as absolute paths.
  const auto deps_contents =
      Immutable("test.o: test.cc header.h"_l) + header_path;

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ(
          (Immutable::Rope{"-E"_l, "-include-pth"_l, preprocessed_header_path,
                           "-dependency-file"_l, deps_path, "-x"_l, language,
                           "-o"_l, "-"_l, input_path}),
          process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path.c_str(),
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{
                    action, "-include-pth"_l, preprocessed_header_path,
                    "-load"_l, plugin_path, "-dependency-file"_l, deps_path,
                    "-x"_l, language, Immutable("-fsanitize-blacklist="_l) +
                                          Immutable(sanitize_blacklist_path),
                    "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path.c_str(),
                                    object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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
    extension->mutable_flags()->add_included_files(preprocessed_header_path);
    extension->mutable_flags()->add_non_cached("-include-pth");
    extension->mutable_flags()->add_non_cached(preprocessed_header_path);
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

    ASSERT_TRUE(base::File::Write(input_path.string_copy(), source_code));
    ASSERT_TRUE(base::File::Write(header_path.string_copy(), header_contents));
    ASSERT_TRUE(base::File::Write(sanitize_blacklist_path.string_copy(),
                                  sanitize_blacklist_contents));

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->MutableExtension(base::proto::Local::extension);
    extension->set_current_dir(temp_dir2);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);
    extension->mutable_flags()->add_non_cached("-include-pth");
    extension->mutable_flags()->add_non_cached(preprocessed_header_path);
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

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  Immutable cache_output;
  const auto output2_path = temp_dir2.GetPath() / output_path.c_str();
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = temp_dir2.GetPath() / deps_path.c_str();
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
  ASSERT_TRUE(base::CreateDirectory(Path(relpath.string_copy())));

  const auto input_path = relpath + "/test.cc"_l;
  const auto header_path = relpath + "/header.h"_l;
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto header_contents2 = "#define B"_l;

  const auto relpath2 = path + "/path2"_l;
  ASSERT_TRUE(base::CreateDirectory(Path(relpath2.string_copy())));

  const auto input_path2 = relpath2 + "/test.cc"_l;
  const auto header_path2 = relpath2 + "/header.h"_l;

  ASSERT_TRUE(base::File::Write(input_path.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header_path.string_copy(), header_contents));

  ASSERT_TRUE(base::File::Write(input_path2.string_copy(), source_code));
  ASSERT_TRUE(base::File::Write(header_path2.string_copy(), header_contents2));

  // Prepare configuration.
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.HasExtension(net::proto::Status::extension));
      const auto& status = message.GetExtension(net::proto::Status::extension);
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path,
                                 "-x"_l, language, "-o"_l, "-"_l, input_path}),
                process->args_);
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path.c_str(),
                                    deps_contents));
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps_path, "-x"_l,
                                 language, "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path.c_str(),
                                    object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-dependency-file"_l, deps_path2,
                                 "-x"_l, language, "-o"_l, "-"_l, input_path2}),
                process->args_);
      process->stdout_ = preprocessed_source2;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path2.c_str(),
                                    deps_contents2));
    } else if (run_count == 4) {
      EXPECT_EQ((Immutable::Rope{action, "-load"_l, plugin_path,
                                 "-dependency-file"_l, deps_path2, "-x"_l,
                                 language, "-o"_l, output_path2, input_path2}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path2.c_str(),
                                    object_code2));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
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
  const auto output2_path = temp_dir.GetPath() / output_path.c_str();
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = temp_dir.GetPath() / deps_path.c_str();
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

  Immutable cache_output2;
  const auto output2_path2 = temp_dir.GetPath() / output_path2.c_str();
  EXPECT_TRUE(base::File::Exists(output2_path2));
  EXPECT_TRUE(base::File::Read(output2_path2, &cache_output2));
  EXPECT_EQ(object_code2, cache_output2);

  Immutable cache_deps2;
  const auto deps2_path2 = temp_dir.GetPath() / deps_path2.c_str();
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
