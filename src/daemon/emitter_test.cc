#include <daemon/emitter.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/platform.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>
#include <cache/blob_store.h>
#include <cache/compression.h>
#include <cache/file_cache.h>
#include <daemon/common_daemon_test.h>
#include <test/throw_std.h>
#include <net/test_connection.h>
#include <perf/stat_service.h>

#include STL(fstream)
#include STL(thread)

#include <sys/socket.h>
#include <sys/stat.h>
#include <utime.h>

namespace dist_clang {
namespace daemon {

// Preprocessing writes the dependency file into a temporary directory.
Immutable::Rope WithoutDepsFile(const base::Process* process) {
  Immutable::Rope args;
  bool skip_next = false;
  for (const auto& arg : process->args()) {
    if (skip_next) {
      skip_next = false;
    } else if (arg == "-MF"_l) {
      skip_next = true;
    } else {
      args.push_back(arg);
    }
  }
  return args;
}

void WriteDepsFile(const base::Process* process, Immutable content) {
  bool next = false;
  for (const auto& arg : process->args()) {
    if (next) {
      EXPECT_TRUE(base::File::Write(arg.string_copy(), content));
      return;
    }
    next = arg == "-MF"_l;
  }
}


TEST(PendingStoresTest, WaitForEarlierStores) {
  PendingStores stores;
  std::optional<PendingStores::Store> earlier(std::in_place, &stores, "a.cc");
  // The stores of the same key don't wait for each other.
  std::optional<PendingStores::Store> same(std::in_place, &stores, "a.cc");
  same.reset();
  // The other keys aren't waited for.
  stores.WaitFor("b.cc");

  std::atomic<bool> waited = false;
  std::thread lookup([&] {
    stores.WaitFor("a.cc");
    waited = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(waited);

  // The lookup doesn't wait for the stores, that begin after it.
  std::optional<PendingStores::Store> later(std::in_place, &stores, "a.cc");
  earlier.reset();
  for (int i = 0; i < 100 && !waited; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(waited);
  later.reset();
  lookup.join();
}

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
    // The most tests expect the preprocessing before the compilation.
    emitter->set_preprocessing(proto::Configuration::Emitter::ALWAYS);

    listen_callback = [this](const String& host, ui16 port, String*) {
      EXPECT_EQ(socket_path, host);
      EXPECT_EQ(0u, port);
      return !::testing::Test::HasNonfatalFailure();
    };

    connect_callback = [this](net::TestConnection* connection, net::EndPointPtr) {
      connection->CallOnSend([this](const net::Connection::Message& message) {
        using net::proto::Status;
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code());
        return !::testing::Test::HasNonfatalFailure();
      });
      return true;
    };
  }

  UniquePtr<Emitter> emitter;
  const String socket_path;
  net::proto::Status::Code expected_code = net::proto::Status::OK;
};

/*
 * We can connect, and when we connect the emitter waits for message.
 */
namespace {

// The files, modified shortly before the start of the compilation, look
// modified during it: the edits of the tests are a bit apart from it.
void WaitAfterEdit() {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

}  // namespace

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
  EXPECT_TRUE(connection.expired()) << "Daemon must not store references to the connection";
}

/*
 * If we receive a bad message from client, then say it back to him.
 */
TEST_F(EmitterTest, BadLocalMessage) {
  expected_code = net::proto::Status::BAD_MESSAGE;

  const String expected_description = "Test description";

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code());
      EXPECT_EQ(expected_description, status.description());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

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
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, LocalMessageWithoutCommand) {
  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_THROW_STD(test_connection->TriggerReadAsync(std::move(message), status), "Assertion failed: false");
  }

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

/*
 * If a client requests compiler version, that we don't have - send NO_VERSION.
 * This test checks |DoLocalExecute()|.
 */
TEST_F(EmitterTest, LocalMessageWithBadCompiler) {
  expected_code = net::proto::Status::NO_VERSION;

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

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

namespace {

// FIXME: maybe make it a lambda?
String EndPointString(const String& host, const ui16 port = 0) {
  return host + ":" + std::to_string(port);
}

}  // namespace

TEST_F(EmitterTest, ConfigurationUpdateFromCoordinator) {
  const base::TemporaryDir temp_dir;
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

  // The coordinator is polled all the time: the connections are told apart by
  // the end points, and the steps are waited for by their own events.
  Atomic<ui32> coordinator_connections = {0}, remote_connections = {0},
               local_connections = {0};
  bool old_remote_used = false;
  ui32 local_replies = 0;
  auto Notify = [&](auto&& change) {
    {
      UniqueLock lock(send_mutex);
      change();
    }
    send_condition.notify_all();
  };

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    // All connection callbacks are on emitter side.

    const String end_point_string = end_point->Print();
    if (end_point_string == EndPointString(coordinator_host, coordinator_port)) {
      // Connection from emitter to coordinator.
      const ui32 number = ++coordinator_connections;
      if (number == 1) {
        connection->CallOnRead([&](net::Connection::Message* message) {
          auto* emitter = message->mutable_configuration()->mutable_emitter();
          auto* remote = emitter->add_remotes();
          remote->set_host(old_remote_host);
          remote->set_port(old_remote_port);
          remote->set_shard(1);
          remote->set_threads(1);

          emitter->set_total_shards(old_total_shards);
        });
      } else if (number == 2) {
        connection->CallOnSend([&](const net::Connection::Message&) {
          // Run the task #1 on old remote before the new configuration.
          UniqueLock lock(send_mutex);
          EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] { return old_remote_used; }));
        });

        connection->CallOnRead([&](net::Connection::Message* message) {
          auto* emitter = message->mutable_configuration()->mutable_emitter();
          auto* remote = emitter->add_remotes();
          remote->set_host(new_remote_host);
          remote->set_port(new_remote_port);
          remote->set_shard(1);
          remote->set_threads(1);

          emitter->set_total_shards(new_total_shards);
        });
      }
      // The next polls follow the updates.
      Notify([] {});
    } else if (end_point_string == EndPointString(socket_path, 0)) {
      // Connection from local client to emitter. Tasks #1 & #2.
      ++local_connections;

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();
        Notify([&] { ++local_replies; });
      });
    } else if (end_point_string == EndPointString(old_remote_host, old_remote_port)) {
      // Connection from emitter to remote absorber. Task #1.
      ++remote_connections;
      EXPECT_EQ(old_total_shards, emitter->conf()->emitter().total_shards());

      connection->CallOnSend([&](const net::Connection::Message&) {
        Notify([&] { old_remote_used = true; });
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_result()->set_obj(object_code);
      });
    } else {
      // Connection from emitter to remote absorber. Task #2.
      ++remote_connections;
      EXPECT_EQ(EndPointString(new_remote_host, new_remote_port), end_point_string);

      DCHECK(emitter);
      EXPECT_EQ(new_total_shards, emitter->conf()->emitter().total_shards());

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_result()->set_obj(object_code);
      });
    }

    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"-E"_l, "-o"_l, "-"_l}), WithoutDepsFile(process));
    if (run_count == 1) {
      process->stdout_ = handled_source1;
    } else if (run_count == 2) {
      process->stdout_ = handled_source2;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  {
    // The second poll of the coordinator follows the first update.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] { return coordinator_connections >= 2; }));
  }

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    auto test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    // The third poll of the coordinator follows the second update.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] {
      return local_replies >= 1 && coordinator_connections >= 3;
    }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    auto test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] { return local_replies >= 2; }));
  }

  emitter.reset();

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(3u, coordinator_connections);
  EXPECT_EQ(2u, remote_connections);
  EXPECT_EQ(2u, local_connections);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, TasksGetReshardedOnConfigurationUpdate) {
  const base::TemporaryDir temp_dir;
  const auto action = "fake_action"_l;
  const auto handled_source = "fake_source1"_l;
  const auto obj_code = "local_compilation_obj_code"_l;
  const String object_code = "fake_object_code";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String output_path = "test.o";
  const String second_output_path = "test2.o";
  const String remote_host_name = "remote_host";
  const String coordinator_host = "coordinator_host";
  const ui16 coordinator_port = 4u;
  const ui32 old_total_shards = 6u;
  const ui32 new_total_shards = 2u;
  const ui32 shard_queue_limit = 3u;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_emitter()->set_poll_interval(1u);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  auto* coordinator = conf.mutable_emitter()->add_coordinators();
  coordinator->set_host(coordinator_host);
  coordinator->set_port(coordinator_port);

  for (ui32 remote = 0; remote < old_total_shards; ++remote) {
    auto* remote_host = conf.mutable_emitter()->add_remotes();
    remote_host->set_host(remote_host_name);
    remote_host->set_port(remote);
    remote_host->set_shard(remote);
    remote_host->set_threads(1);
  }

  conf.mutable_emitter()->set_total_shards(old_total_shards);
  conf.mutable_emitter()->set_shard_queue_limit(shard_queue_limit);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  // The coordinator is polled all the time: the connections are told apart by
  // the end points, and the steps are waited for by their own counters.
  Atomic<ui32> coordinator_connections = {0}, remote_connections = {0},
               local_connections = {0};
  bool first_remote_sent = false;
  ui32 local_replies = 0;

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    // All connection callbacks are on emitter side.

    const String end_point_string = end_point->Print();
    if (end_point_string == EndPointString(coordinator_host, coordinator_port)) {
      // Connection from emitter to coordinator: only the first one updates
      // the configuration.
      if (coordinator_connections++) {
        return true;
      }

      connection->CallOnSend([&](const net::Connection::Message&) {
        // The first task is sent to the remote of the old configuration.
        UniqueLock lock(send_mutex);
        EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] { return first_remote_sent; }));
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        auto* emitter = message->mutable_configuration()->mutable_emitter();
        for (ui32 remote = 0; remote < new_total_shards; ++remote) {
          auto* remote_host = emitter->add_remotes();
          remote_host->set_host(remote_host_name);
          remote_host->set_port(remote);
          remote_host->set_shard(remote);
          remote_host->set_threads(1);
        }

        emitter->set_total_shards(new_total_shards);
        emitter->set_shard_queue_limit(shard_queue_limit);
      });
    } else if (end_point_string == EndPointString(socket_path, 0)) {
      // Connection from local client to emitter. Tasks #1 & #2.
      ++local_connections;

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        // The first reply is for the second task: it resumes |DoLocalExecute|
        // for the first one. The second reply finishes the test.
        {
          UniqueLock lock(send_mutex);
          ++local_replies;
        }
        send_condition.notify_all();
      });
    } else if (remote_connections++ == 0) {
      // Connection from emitter to remote absorber. Task #1.

      connection->CallOnSend([&, end_point](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& outgoing = message.remote();
        auto handled_hash = Immutable::WrapString(outgoing.handled_hash());

        const ui32 expected_shard = Emitter::CalculateShard(cache::string::HandledHash(handled_hash), old_total_shards);

        EXPECT_EQ(EndPointString(remote_host_name, expected_shard), end_point->Print());

        ASSERT_LT(new_total_shards, expected_shard) << "Ensure that shard is greater than new total shards we're "
                                                       "sending in new configuration, to make sure the second task "
                                                       "will be redistributed.";

        {
          UniqueLock lock(send_mutex);
          first_remote_sent = true;
        }
        send_condition.notify_all();
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        // Do not send any code to prevent task result appearing in cache to
        // make sure next task with same handled source code goes to remote.

        // Make sure coordinator thread starts new pool, stops
        // current and redistributes tasks.
        // Cant wait here as coordinator should join this thread.
        std::this_thread::sleep_for(Seconds(1));
      });
    } else {
      // Connection from emitter to remote absorber. Task #2.

      DCHECK(emitter);

      connection->CallOnSend([&, end_point](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& outgoing = message.remote();
        auto handled_hash = Immutable::WrapString(outgoing.handled_hash());

        const ui32 expected_shard = Emitter::CalculateShard(cache::string::HandledHash(handled_hash), new_total_shards);

        EXPECT_EQ(EndPointString(remote_host_name, expected_shard), end_point->Print());
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_result()->set_obj(object_code);
      });
    }

    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1 || run_count == 2) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-o"_l, "-"_l}), WithoutDepsFile(process));
      process->stdout_ = handled_source;
    } else if (run_count == 3) {
      // Either task may be the first one.
      const auto args = WithoutDepsFile(process);
      EXPECT_TRUE(args == (Immutable::Rope{"fake_action"_l, "-o"_l, Immutable(output_path)}) ||
                  args == (Immutable::Rope{"fake_action"_l, "-o"_l, Immutable(second_output_path)}));
      // Block until emitter sends result for second task.
      UniqueLock lock(send_mutex);
      EXPECT_TRUE(send_condition.wait_for(lock, Seconds(10), [&] { return local_replies >= 1; }));
      process->stdout_ = obj_code;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    auto test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    auto test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    // The other output: the first task locks its own while it's compiled.
    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(second_output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    // The remote of the first task sleeps a second to apply the new
    // configuration: then the second task is sent to the new remote, and the
    // first one is compiled locally.
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(20), [&] { return local_replies >= 2; }));
  }

  emitter.reset();

  EXPECT_EQ(3u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_LE(1u, coordinator_connections);
  EXPECT_EQ(2u, remote_connections);
  EXPECT_EQ(2u, local_connections);
  EXPECT_EQ(connections_created, read_count);
  EXPECT_EQ(connections_created, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";
}

// Check that while strict sharding if remote fails, it's tasks get
// redistributed between other shards.
TEST_F(EmitterTest, TasksGetReshardedOnFailedRemote) {
  const base::TemporaryDir temp_dir;
  const auto action = "fake_action"_l;
  const auto handled_source = "fake_source"_l;
  const auto object_code = "fake_object_code"_l;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String output_path = "test.o";
  const String remote_host_name = "remote_host";
  const ui32 total_shards = 100u;
  const ui32 shard_queue_limit = 3u;

  conf.mutable_emitter()->set_only_failed(true);
  conf.mutable_cache()->set_path(temp_dir);
  conf.mutable_cache()->set_direct(false);
  conf.mutable_cache()->set_clean_period(1);

  for (ui32 remote = 0; remote < total_shards; ++remote) {
    auto* remote_host = conf.mutable_emitter()->add_remotes();
    remote_host->set_host(remote_host_name);
    remote_host->set_port(remote);
    remote_host->set_shard(remote);
    remote_host->set_threads(1);
  }

  conf.mutable_emitter()->set_total_shards(total_shards);
  conf.mutable_emitter()->set_shard_queue_limit(shard_queue_limit);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  // Initial shard for task. We obtain this value from first connection from
  // emitter to absorber and abort that connection. Than wait for second
  // connection from emitter to another(!) remote and check that remote is from
  // another shard.
  // The shard of the key of the task.
  Atomic<ui32> initial_shard = {36};

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    // All connection callbacks are on emitter side.
    if (connect_count == 1) {
      // Connection from local client to emitter. Tasks #1.

      EXPECT_EQ(EndPointString(socket_path, 0), end_point->Print());

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        // Emitter sent result for task, so reset emitter now and finish test.
        send_condition.notify_one();
      });
    } else if (connect_count == 2) {
      // Connection from emitter to remote absorber. Task #1.

      EXPECT_EQ(EndPointString(remote_host_name, initial_shard), end_point->Print());

      // Abort connection to make emitter choose another shard for task.
      return false;
    } else if (connect_count == 3) {
      // Connection from emitter to remote absorber. Task #2.

      EXPECT_NE(EndPointString(remote_host_name, initial_shard), end_point->Print());

      // Verified that task was redistributed to another shard.
      // Now let's abort connection once again to make sure task donesn't get
      // redistributed and local execution used as fallback.
      return false;
    }

    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-o"_l, "-"_l}), WithoutDepsFile(process));
      process->stdout_ = handled_source;
    } else if (run_count == 2) {
      process->stdout_ = object_code;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    auto test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    auto message = std::make_unique<net::Connection::Message>();
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    auto* flags = extension->mutable_flags();
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);
    flags->set_output(output_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  UniqueLock lock(send_mutex);
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [this] { return send_count == 1u; }));
  emitter.reset();

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(connections_created, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host1, bad_coordinator_port1));
      return false;
    }
    if (connections_created == 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host2, bad_coordinator_port2));
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(2), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      return false;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead(
          [&](net::Connection::Message* message) { message->mutable_configuration(); });
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(2), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      connection->AbortOnSend();
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead(
          [&](net::Connection::Message* message) { message->mutable_configuration(); });
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(3), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      connection->AbortOnRead();
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead(
          [&](net::Connection::Message* message) { message->mutable_configuration(); });
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(3), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {});
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead(
          [&](net::Connection::Message* message) { message->mutable_configuration(); });
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(3), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead(
          [&](net::Connection::Message* message) { message->mutable_configuration(); });
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->mutable_configuration()->mutable_emitter()->add_remotes();
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    if (connections_created == 1) {
      EXPECT_EQ(end_point->Print(), EndPointString(bad_coordinator_host, bad_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->mutable_configuration()->mutable_emitter()->add_remotes();
        remote->set_host(remote_host);
        remote->set_port(remote_port);
        remote->set_disabled(true);
      });
      return true;
    }
    if (connections_created >= 2) {
      EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
      connection->CallOnRead([&](net::Connection::Message* message) {
        auto remote = message->mutable_configuration()->mutable_emitter()->add_remotes();
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [this] { return connections_created > 2; }));

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

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr end_point) {
    EXPECT_EQ(end_point->Print(), EndPointString(good_coordinator_host, good_coordinator_port));
    connection->CallOnRead([&](net::Connection::Message* message) {
      auto remote = message->mutable_configuration()->mutable_emitter()->add_remotes();
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
  EXPECT_TRUE(send_condition.wait_for(lock, Seconds(4), [this] { return connections_created > 1; }));

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
  expected_code = net::proto::Status::NO_VERSION;

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

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, LocalMessageWithBadPlugin) {
  expected_code = net::proto::Status::NO_VERSION;

  const String compiler_version = "1.0";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";
  const String bad_plugin_name = "bad_plugin_name";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, LocalMessageWithBadPlugin2) {
  expected_code = net::proto::Status::NO_VERSION;

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

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(bad_plugin_name);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(0u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, LocalMessageWithPluginPath) {
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

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(Path(compiler_path), process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path}), process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, LocalMessageWithSanitizeBlacklist) {
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const auto action = "fake_action"_l;
  const auto sanitize_blacklist_path = "asan-blacklist.txt"_l;
  const ui32 user_id = 1234;

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(Path(compiler_path), process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, Immutable("-fsanitize-blacklist="_l) + sanitize_blacklist_path}),
              process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_sanitize_blacklist(sanitize_blacklist_path);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, ConfigurationWithoutVersions) {
  const String compiler_version = "fake_compiler_version";
  const auto compiler_path = "fake_compiler_path"_l;
  const String current_dir = "fake_current_dir";
  const String plugin_name = "fake_plugin";
  const auto plugin_path = "fake_plugin_path"_l;
  const auto action = "fake_action"_l;
  const ui32 user_id = 1234;

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(Path(compiler_path), process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path}), process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, LocalSuccessfulCompilation) {
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

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(Path(compiler_path), process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path}), process->args_);
    EXPECT_EQ(user_id, process->uid_);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    extension->set_user_id(user_id);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";

  // TODO: check absolute output path.
}

TEST_F(EmitterTest, DISABLED_RemoteSuccessfulCompilation) {
  // TODO: implement this test.
  //       - Check the permissions of object and deps files, if the client
  //         provides the user_id.
  //       - Check the outgoing message doesn't have compiler and plugins paths.
}

TEST_F(EmitterTest, LocalFailedCompilation) {
  expected_code = net::proto::Status::EXECUTION;

  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const String current_dir = "fake_current_dir";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  do_run = false;

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1, connection.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, LocalCompilationNotStarted) {
  // The client compiles by itself: the compiler hasn't failed.
  expected_code = net::proto::Status::OVERLOAD;

  const String compiler_version = "fake_compiler_version";

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  // E.g. "fork()" fails.
  do_run = false;
  run_callback = [](base::TestProcess* process) { process->exited_ = false; };

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      EXPECT_EQ(expected_code, message.status().code());
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
    auto* extension = message->mutable_local();
    extension->set_current_dir("fake_current_dir");
    extension->mutable_flags()->mutable_compiler()->set_version(
        compiler_version);
    extension->mutable_flags()->set_action("fake_action");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, send_count);
}

/*
 * 1. Store new entry in the cache after a local compilation.
 * 2. Try to compile the same preprocessed source locally.
 * 3. Restore saved entry from the cache without compilation.
 */
TEST_F(EmitterTest, StoreSimpleCacheForLocalResult) {
  const base::TemporaryDir temp_dir;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  // The preprocessed source has the line markers - of the files to check.
  const auto source = "# 1 \"test1.cc\"\nfake_source"_l;
  const auto language = "fake_language"_l;
  const auto action = "fake_action"_l;
  const auto input_path1 = "test1.cc"_l;
  const auto input_path2 = "test2.cc"_l;
  // The inputs are checked for modifications after the compilation.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / input_path1, "1"_l));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / input_path2, "2"_l));
  const auto output_path1 = "test1.o"_l;
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;

  const auto output_path2 = temp_dir.path() / "test2.o";
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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path1}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language, "-o"_l, output_path1, input_path1}),
                WithoutDepsFile(process));
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1, object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path2}), WithoutDepsFile(process));
      process->stdout_ = source;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
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
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, StoreSimpleCacheForRemoteResult) {
  const base::TemporaryDir temp_dir;
  const String host = "fake_host";
  const ui16 port = 12345;
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto object_code = "fake_object_code"_l;
  const auto source = "fake_source"_l;
  const auto language = "fake_language"_l;
  const String action = "fake_action";
  const auto input_path1 = "test1.cc"_l;
  const auto input_path2 = "test2.cc"_l;
  // The inputs are checked for modifications after the compilation.
  ASSERT_TRUE(base::File::Write(temp_dir.path() / input_path1, "1"_l));
  ASSERT_TRUE(base::File::Write(temp_dir.path() / input_path2, "2"_l));
  const String output_path1 = "test1.o";
  const auto plugin_name = "fake_plugin"_l;
  const auto plugin_path = "fake_plugin_path"_l;
  const auto remote_compilation_time = std::chrono::milliseconds(10);

  const auto output_path2 = temp_dir.path() / "test2.o";
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
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& command = message.remote();
        EXPECT_EQ(source, command.source());

        send_condition.notify_all();
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        // Used to check remote compilation timing statistics.
        std::this_thread::sleep_for(remote_compilation_time);
        message->mutable_result()->set_obj(object_code);
      });
    } else if (connect_count == 3) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path1}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path2}), WithoutDepsFile(process));
      process->stdout_ = source;
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
    // FIXME: describe, why |send_count == 2| ?
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 3; }));
  }

  emitter.reset();

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::SIMPLE_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  perf::proto::Metric remote_compilation_time_metric;
  remote_compilation_time_metric.set_name(perf::proto::Metric::REMOTE_COMPILATION_TIME);
  base::Singleton<perf::StatService>::Get().Dump(remote_compilation_time_metric);
  EXPECT_GE(remote_compilation_time_metric.value(), static_cast<ui64>(remote_compilation_time.count()));

  Immutable cache_output;
  ASSERT_TRUE(base::File::Exists(output_path2));
  ASSERT_TRUE(base::File::Read(output_path2, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(3u, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(3u, read_count);
  EXPECT_EQ(3u, send_count) << "There should be only these transmissions:" << std::endl
                            << "  1. Local daemon -> remote daemon." << std::endl
                            << "  2. Local daemon -> 1st client." << std::endl
                            << "  3. Local daemon -> 2nd client.";
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

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
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& command = message.remote();
        EXPECT_EQ(source, command.source());

        EXPECT_TRUE(command.flags().rewrite_includes());

        send_condition.notify_all();
      });
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_status()->set_code(net::proto::Status::EXECUTION);
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path1}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 2) {
      // Used to check local compilation timing statistics.
      std::this_thread::sleep_for(local_compilation_time);
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language, "-o"_l, output_path1, input_path1}),
                process->args_);
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1, object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();

    extension->set_current_dir(temp_dir);
    extension->mutable_flags()->set_input(input_path1);
    extension->mutable_flags()->set_output(output_path1);
    extension->mutable_flags()->set_rewrite_includes(true);
    auto* compiler = extension->mutable_flags()->mutable_compiler();
    compiler->set_version(compiler_version);
    compiler->add_plugins()->set_name(plugin_name);
    extension->mutable_flags()->set_action(action);
    extension->mutable_flags()->set_language(language);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
    // FIXME: describe, why |send_count == 2| ?
  }

  emitter.reset();

  perf::proto::Metric remote_compilation_failed_metric;
  remote_compilation_failed_metric.set_name(perf::proto::Metric::REMOTE_COMPILATION_FAILED);
  base::Singleton<perf::StatService>::Get().Dump(remote_compilation_failed_metric);
  EXPECT_EQ(1u, remote_compilation_failed_metric.value());

  perf::proto::Metric local_compilation_time_metric;
  local_compilation_time_metric.set_name(perf::proto::Metric::LOCAL_COMPILATION_TIME);
  base::Singleton<perf::StatService>::Get().Dump(local_compilation_time_metric);
  EXPECT_GE(local_compilation_time_metric.value(), static_cast<ui64>(local_compilation_time.count()));

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count) << "There should be only two transmissions:" << std::endl
                            << "  1. From client -> local daemon." << std::endl
                            << "  2. Local daemon -> remote daemon." << std::endl;
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
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
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& command = message.remote();
        EXPECT_EQ(source, command.source());

        send_condition.notify_all();
      });
      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_status()->set_code(net::proto::Status::OVERLOAD);
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      // Used to check preprocess timing statistics.
      std::this_thread::sleep_for(preprocess_time);
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path1}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language, "-o"_l, output_path1, input_path1}),
                WithoutDepsFile(process));
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path1, object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
    // FIXME: describe, why |send_count == 2| ?
  }

  emitter.reset();

  perf::proto::Metric remote_compilation_failed_metric;
  remote_compilation_failed_metric.set_name(perf::proto::Metric::REMOTE_COMPILATION_FAILED);
  base::Singleton<perf::StatService>::Get().Dump(remote_compilation_failed_metric);
  EXPECT_EQ(0u, remote_compilation_failed_metric.value());

  perf::proto::Metric remote_compilation_rejected_metric;
  remote_compilation_rejected_metric.set_name(perf::proto::Metric::REMOTE_COMPILATION_REJECTED);
  base::Singleton<perf::StatService>::Get().Dump(remote_compilation_rejected_metric);
  EXPECT_EQ(1u, remote_compilation_rejected_metric.value());

  perf::proto::Metric preprocess_time_metric;
  preprocess_time_metric.set_name(perf::proto::Metric::PREPROCESS_TIME);
  base::Singleton<perf::StatService>::Get().Dump(preprocess_time_metric);
  EXPECT_GE(preprocess_time_metric.value(), static_cast<ui64>(preprocess_time.count()));

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count) << "There should be only these transmissions:" << std::endl
                            << "  1. Local daemon -> remote daemon." << std::endl
                            << "  2. Local daemon -> client." << std::endl;
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, StoreSimpleCacheForLocalResultWithAndWithoutBlacklist) {
  const base::TemporaryDir temp_dir;
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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language, "-o"_l, output_path1, input_path}),
                WithoutDepsFile(process));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}), WithoutDepsFile(process));
      process->stdout_ = source;
    } else if (run_count == 4) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language,
                                 Immutable("-fsanitize-blacklist="_l) + sanitize_blacklist_path, "-o"_l, output_path2,
                                 input_path}),
                process->args_);
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  emitter.reset();

  EXPECT_EQ(4u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, SkipPreprocessingOfEditedSource) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto action = "fake_action"_l;

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  ui32 preprocessed_count = 0;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    runs.push_back(preprocessing ? "-E" : process->args_.front().string_copy());
    const auto deps = Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                      Immutable(header_path);
    if (preprocessing) {
      // Every preprocessing gives a new source - no simple cache hits.
      process->stdout_ = Immutable(String("preprocessed ") +
                                   std::to_string(++preprocessed_count));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    } else {
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The first compilation of the source - preprocess and compile.
  Compile(1);
  EXPECT_EQ((List<String>{"-E", "fake_action"}), runs);

  // The edited source - compile only.
  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 2; }"_l));
  WaitAfterEdit();
  runs.clear();
  Compile(2);
  EXPECT_EQ((List<String>{"fake_action"}), runs);

  // The result is found by the direct cache.
  ASSERT_TRUE(base::File::Delete(output_path));
  runs.clear();
  Compile(3);
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));

  // Only the headers changed - preprocess again.
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  runs.clear();
  Compile(4);
  EXPECT_EQ((List<String>{"-E", "fake_action"}), runs);

  emitter.reset();
}

TEST_F(EmitterTest, DontHitDirectCacheForSameContentsOfAnotherSource) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto input_path2 = temp_dir.path() / "test2.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto action = "fake_action"_l;

  // The same contents - but "__FILE__" and the debug info differ.
  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(input_path2, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  ui32 preprocessed_count = 0;
  String current_input;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    runs.push_back(preprocessing ? "-E" : process->args_.front().string_copy());
    const auto deps = Immutable("test.o: "_l) + Immutable(current_input);
    if (preprocessing) {
      process->stdout_ = Immutable(String("preprocessed ") +
                                   std::to_string(++preprocessed_count));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    } else {
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](const String& input, ui32 expected_send_count) {
    current_input = input;
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  Compile(input_path, 1);
  EXPECT_EQ((List<String>{"-E", "fake_action"}), runs);

  // The same source is found.
  runs.clear();
  Compile(input_path, 2);
  EXPECT_TRUE(runs.empty());

  // Another source with the same contents is compiled.
  runs.clear();
  Compile(input_path2, 3);
  EXPECT_EQ((List<String>{"-E", "fake_action"}), runs);

  emitter.reset();
}

TEST_F(EmitterTest, VerifyHits) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto dwo_path = temp_dir.path() / "test.dwo";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";
  const auto action = "fake_action"_l;

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_verify_hits_percent(100);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  // The compiler gives this result.
  Mutex object_mutex;
  String object = "object";
  List<String> runs;
  ui32 preprocessed_count = 0;
  run_callback = [&](base::TestProcess* process) {
    const Vector<String> args(process->args_.begin(), process->args_.end());
    if (std::find(args.begin(), args.end(), "-E") != args.end()) {
      process->stdout_ = Immutable(String("preprocessed ") +
                                   std::to_string(++preprocessed_count));
      WriteDepsFile(process, Immutable("test.o: "_l) + Immutable(input_path));
      std::lock_guard<Mutex> lock(object_mutex);
      runs.push_back("-E");
      return;
    }

    String output, split_dwarf_output, split_dwarf_file;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "-o") {
        output = args[i + 1];
      } else if (args[i] == "-split-dwarf-output") {
        split_dwarf_output = args[i + 2];
      } else if (args[i] == "-split-dwarf-file") {
        split_dwarf_file = args[i + 2];
      }
    }
    std::lock_guard<Mutex> lock(object_mutex);
    const bool verification = output != String(output_path);
    runs.push_back(verification ? "verify" : "compile");
    if (verification) {
      // Nothing is written into the build directory - but the object refers
      // to the original split DWARF file.
      EXPECT_EQ(Path(output).parent_path() / "output.dwo", split_dwarf_output);
      EXPECT_EQ("test.dwo", split_dwarf_file);
      EXPECT_EQ(19, process->nice_);
    } else {
      EXPECT_TRUE(split_dwarf_output.empty());
      EXPECT_EQ(0, process->nice_);
    }
    EXPECT_TRUE(base::File::Write(output, Immutable(object)));
    EXPECT_TRUE(base::File::Write(
        verification ? Path(output).parent_path() / "output.dwo" : dwo_path,
        "dwo"_l));
    const auto deps = Immutable("test.o: "_l) + Immutable(input_path);
    if (verification) {
      WriteDepsFile(process, deps);
    } else {
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->set_dwo_file("test.dwo");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The dump resets the metric.
  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };
  auto WaitForStat = [&](perf::proto::Metric::Name name, ui64 value) {
    for (int i = 0; i < 200 && Stat(name) < value; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Stat(name);
  };
  auto Runs = [&] {
    std::lock_guard<Mutex> lock(object_mutex);
    auto result = runs;
    runs.clear();
    return result;
  };

  Compile(1);
  EXPECT_EQ((List<String>{"-E", "compile"}), Runs());

  // The same result.
  Compile(2);
  EXPECT_EQ(1u, WaitForStat(perf::proto::Metric::HIT_VERIFIED, 1));
  EXPECT_EQ((List<String>{"verify"}), Runs());
  EXPECT_EQ(0u, Stat(perf::proto::Metric::HIT_VERIFICATION_FAILED));
  Immutable content;
  ASSERT_TRUE(base::File::Read(output_path, &content));
  EXPECT_EQ("object"_l, content);

  // Something, that isn't a part of the key, changes the result.
  {
    std::lock_guard<Mutex> lock(object_mutex);
    object = "another object";
  }
  Compile(3);
  EXPECT_EQ(1u, WaitForStat(perf::proto::Metric::HIT_VERIFICATION_FAILED, 1));
  EXPECT_EQ((List<String>{"verify"}), Runs());
  // The wrong result in the build directory is replaced with the right one.
  Immutable verified_content;
  ASSERT_TRUE(base::File::Read(output_path, &verified_content));
  EXPECT_EQ("another object"_l, verified_content);

  // The wrong entry is removed - in background.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Compile(4);
  EXPECT_EQ((List<String>{"-E", "compile"}), Runs());
  Immutable new_content;
  ASSERT_TRUE(base::File::Read(output_path, &new_content));
  EXPECT_EQ("another object"_l, new_content);

  emitter.reset();
}

TEST_F(EmitterTest, VerifyHitsWithoutDeps) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "test.h";
  const auto output_path = temp_dir.path() / "test.o";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "int a;"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_verify_hits_percent(100);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      send_condition.notify_all();
    });
    return true;
  };

  Mutex runs_mutex;
  List<String> runs;
  run_callback = [&](base::TestProcess* process) {
    const Vector<String> args(process->args_.begin(), process->args_.end());
    const Immutable input(input_path), header(header_path);
    std::lock_guard<Mutex> lock(runs_mutex);
    if (std::find(args.begin(), args.end(), "-E") != args.end()) {
      process->stdout_ = Immutable("# 1 \""_l) + input + "\"\n# 1 \""_l +
                         header + "\" 1\nint a;\n# 2 \""_l + input +
                         "\" 2\n"_l;
      runs.push_back("-E");
      return;
    }

    String output;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "-o") {
        output = args[i + 1];
      }
    }
    const bool verification = output != String(output_path);
    runs.push_back(verification ? "verify" : "compile");
    // The header is edited after the hit - before the first verification.
    const bool edited =
        verification && std::count(runs.begin(), runs.end(), "verify") == 1;
    if (edited) {
      EXPECT_TRUE(base::File::Write(header_path, "int b;"_l));
    }
    EXPECT_TRUE(
        base::File::Write(output, edited ? "another object"_l : "object"_l));
    WriteDepsFile(process, Immutable("test.o: "_l) + input + " "_l + header);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The dump resets the metric.
  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };
  auto WaitForStat = [&](perf::proto::Metric::Name name, ui64 value) {
    for (int i = 0; i < 200 && Stat(name) < value; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Stat(name);
  };
  auto Runs = [&] {
    std::lock_guard<Mutex> lock(runs_mutex);
    auto result = runs;
    return result;
  };

  Compile(1);
  EXPECT_EQ((List<String>{"-E", "compile"}), Runs());

  // The header is edited before the verification: the different result
  // doesn't mean, that the entry is wrong.
  Compile(2);
  for (int i = 0; i < 200 && Runs().size() < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(0u, Stat(perf::proto::Metric::HIT_VERIFICATION_FAILED));
  EXPECT_EQ(0u, Stat(perf::proto::Metric::HIT_VERIFIED));

  // The entry is kept.
  Compile(3);
  EXPECT_EQ(1u, WaitForStat(perf::proto::Metric::HIT_VERIFIED, 1));
  EXPECT_EQ((List<String>{"-E", "compile", "-E", "verify", "-E", "verify"}),
            Runs());

  emitter.reset();
}

TEST_F(EmitterTest, NeverPreprocess) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::NEVER);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    runs.push_back(preprocessing ? "-E" : "compile");
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                       Immutable(header_path)));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  Compile(1);
  EXPECT_EQ((List<String>{"compile"}), runs);

  runs.clear();
  ASSERT_TRUE(base::File::Delete(output_path));
  Compile(2);
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));

  runs.clear();
  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  Compile(3);
  EXPECT_EQ((List<String>{"compile"}), runs);

  emitter.reset();
}

// The preprocessed source doesn't have the macros, that the precompiled header
// has, and the verification compiles it from another root.
TEST_F(EmitterTest, PrecompiledHeaderOnlyInDirectCache) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "pch.h";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "pch.h.pch";
  const auto deps_path = temp_dir.path() / "pch.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "#include \"header.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::ALWAYS);
  conf.mutable_emitter()->set_verify_hits_percent(100);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  Mutex runs_mutex;
  List<String> runs;
  run_callback = [&](base::TestProcess* process) {
    Vector<String> args;
    for (const auto& arg : process->args()) {
      args.push_back(arg.string_copy());
    }
    const bool preprocessing =
        std::find(args.begin(), args.end(), "-E") != args.end();
    String output;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "-o") {
        output = args[i + 1];
      }
    }
    {
      std::lock_guard<Mutex> lock(runs_mutex);
      runs.push_back(preprocessing                ? "-E"
                     : output == String(output_path) ? "compile"
                                                     : "verify");
    }
    if (!preprocessing) {
      EXPECT_TRUE(base::File::Write(output, "pch"_l));
      EXPECT_TRUE(base::File::Write(
          deps_path, Immutable("pch.h.pch: "_l) + Immutable(input_path) +
                         " "_l + Immutable(header_path)));
    }
  };
  auto Runs = [&] {
    std::lock_guard<Mutex> lock(runs_mutex);
    auto result = runs;
    return result;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("pch.h.pch");
    flags->set_language("c++-header");
    flags->set_pch(true);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  Compile(1);
  EXPECT_EQ((List<String>{"compile"}), Runs());

  ASSERT_TRUE(base::File::Delete(output_path));
  Compile(2);
  EXPECT_TRUE(base::File::Exists(output_path));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ((List<String>{"compile"}), Runs());

  ASSERT_TRUE(base::File::Write(header_path, "#define B"_l));
  Compile(3);
  EXPECT_EQ((List<String>{"compile", "compile"}), Runs());

  emitter.reset();
}

// The precompiled header, that is shared between the roots, mustn't have the
// paths of its own root.
TEST_F(EmitterTest, DontStorePrecompiledHeaderWithRoot) {
  const base::TemporaryDir temp_dir;
  const String root = temp_dir.path();
  const auto build_dir = temp_dir.path() / "out";
  const auto input_path = temp_dir.path() / "pch.h";
  const auto output_path = build_dir / "pch.h.pch";
  const auto deps_path = build_dir / "pch.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::CreateDirectory(build_dir));
  ASSERT_TRUE(base::File::Write(input_path, "#define A"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_cache()->add_source_roots(root);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  String content = "pch of " + root;
  ui32 runs = 0;
  run_callback = [&](base::TestProcess* process) {
    ++runs;
    EXPECT_TRUE(
        base::File::Write(output_path, Immutable::WrapString(content)));
    EXPECT_TRUE(base::File::Write(deps_path, "pch.h.pch: ../pch.h"_l));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(build_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input("../pch.h");
    flags->set_output("pch.h.pch");
    flags->set_deps_file("pch.d");
    flags->set_deps_target("pch.h.pch");
    flags->set_language("c++-header");
    flags->set_pch(true);
    flags->set_pch_sysroot(root + "/");
    flags->set_relocatable(true);
    flags->add_remapped_prefixes(root);
    // The flags of the test aren't prepared by the emitter.
    flags->set_key_root(root);
    flags->set_key_root_relative("..");
    flags->set_key_root_remapped(true);
    flags->add_key_extra("pch-cwd:out");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  Compile(1);
  Compile(2);
  EXPECT_EQ(2u, runs);

  // Without the root it's stored.
  content = "pch";
  Compile(3);
  Compile(4);
  EXPECT_EQ(3u, runs);

  emitter.reset();
}

TEST_F(EmitterTest, UpdateCacheWithoutLookup) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  WaitAfterEdit();

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  bool traced = false;
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      runs.push_back("-E");
      process->stdout_ = "preprocessed"_l;
      return;
    }
    runs.push_back("compile");
    traced = std::find(process->args_.begin(), process->args_.end(),
                       "-ftime-trace"_l) != process->args_.end();
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  enum class Mode { NORMAL, TIME_TRACE, RECACHE };
  auto Compile = [&](Mode mode, ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    if (mode == Mode::TIME_TRACE) {
      flags->add_ignored()->add_values("-ftime-trace");
      flags->set_time_trace(true);
    }
    if (mode == Mode::RECACHE) {
      extension->add_env("DC_RECACHE=1");
    }
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // Neither the preprocessing, nor the hit: the trace is produced only by the
  // compilation.
  Compile(Mode::TIME_TRACE, 1);
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_TRUE(traced);

  runs.clear();
  Compile(Mode::TIME_TRACE, 2);
  EXPECT_EQ((List<String>{"compile"}), runs);

  // But the result is stored - for the compilations without the trace.
  runs.clear();
  ASSERT_TRUE(base::File::Delete(output_path));
  Compile(Mode::NORMAL, 3);
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));

  // The same on the request of the client - e.g. to measure the build.
  Compile(Mode::RECACHE, 4);
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_FALSE(traced);

  runs.clear();
  ASSERT_TRUE(base::File::Delete(output_path));
  Compile(Mode::NORMAL, 5);
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));

  emitter.reset();
}

TEST_F(EmitterTest, StoreTimeTraces) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto trace_path = temp_dir.path() / "test.json";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  WaitAfterEdit();

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_cache()->set_store_time_traces(true);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  ui32 compilations = 0;
  bool writes_trace = true;
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      runs.push_back("-E");
      process->stdout_ = "preprocessed"_l;
      return;
    }
    runs.push_back("compile");
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
    if (writes_trace &&
        std::find(process->args_.begin(), process->args_.end(),
                  "-ftime-trace"_l) != process->args_.end()) {
      EXPECT_TRUE(base::File::Write(
          trace_path, Immutable("trace " + std::to_string(++compilations))));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  ui32 send_count_expected = 0;
  auto Compile = [&](const char* granularity) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    if (granularity) {
      flags->add_ignored()->add_values("-ftime-trace");
      if (*granularity) {
        flags->add_ignored()->add_values(granularity);
      }
      flags->set_time_trace(true);
      flags->set_time_trace_file("test.json");
    }
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    ++send_count_expected;
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == send_count_expected;
    }));
  };
  auto Trace = [&] {
    Immutable trace;
    base::File::Read(trace_path, &trace);
    return trace.string_copy();
  };
  auto Clear = [&] {
    runs.clear();
    base::File::Delete(output_path);
    base::File::Delete(trace_path);
  };

  // The entry without the trace isn't found by the compilation with it.
  Compile(nullptr);
  EXPECT_EQ((List<String>{"-E", "compile"}), runs);
  Clear();
  Compile("");
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_EQ("trace 1", Trace());

  // The trace is restored with the object.
  Clear();
  Compile("");
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));
  EXPECT_EQ("trace 1", Trace());

  // But not the one with other options.
  Clear();
  Compile("-ftime-trace-granularity=100");
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_EQ("trace 2", Trace());

  // The compilation without the trace doesn't need it.
  Clear();
  Compile(nullptr);
  EXPECT_TRUE(runs.empty());
  EXPECT_TRUE(base::File::Exists(output_path));
  EXPECT_FALSE(base::File::Exists(trace_path));

  // The stale trace isn't taken for the one of the compilation.
  Clear();
  writes_trace = false;
  ASSERT_TRUE(base::File::Write(trace_path, "stale"_l));
  Compile("-ftime-trace-granularity=200");
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_FALSE(base::File::Exists(trace_path));
  writes_trace = true;
  Clear();
  Compile("-ftime-trace-granularity=200");
  EXPECT_EQ((List<String>{"compile"}), runs);
  EXPECT_EQ("trace 3", Trace());

  emitter.reset();
}

TEST_F(EmitterTest, PreprocessWhenIdle) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A 1"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::WHEN_IDLE);
  // The unchanged outputs keep their time - like for Ninja with "restat".
  conf.mutable_emitter()->set_keep_unchanged_outputs(true);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      send_condition.notify_all();
    });
    return true;
  };

  // The preprocessed source and the object, that the compiler gives.
  Mutex mutex;
  String preprocessed, object;
  Atomic<bool> wait_for_preprocessing = false, wait_for_cancel = false;
  Atomic<bool> preprocessing_done = false, compiling = false;
  Atomic<ui32> compilations = 0, canceled = 0;
  const auto deps = Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                    Immutable(header_path);
  auto WaitFor = [](const Fn<bool()>& condition) {
    for (int i = 0; i < 300 && !condition(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
  };
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      if (wait_for_cancel) {
        // The hit cancels the compilation, that runs already.
        EXPECT_TRUE(WaitFor([&] { return bool(compiling); }));
      }
      std::lock_guard<Mutex> lock(mutex);
      process->stdout_ = Immutable(String(preprocessed));
      WriteDepsFile(process, deps);
      preprocessing_done = true;
      return;
    }

    ++compilations;
    compiling = true;
    if (wait_for_cancel) {
      // A long compilation - canceled by the hit of the simple cache.
      EXPECT_TRUE(WaitFor([&] { return process->cancel_requested(); }));
      ++canceled;
      return;
    }
    if (wait_for_preprocessing) {
      EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_done); }));
    }
    std::lock_guard<Mutex> lock(mutex);
    EXPECT_TRUE(base::File::Write(output_path, Immutable(String(object))));
    EXPECT_TRUE(base::File::Write(deps_path, deps));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    preprocessing_done = false;
    compiling = false;
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
  };

  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };
  // The hit is counted after the reply.
  auto WaitForStat = [&Stat](perf::proto::Metric::Name name, ui64 value) {
    for (int i = 0; i < 300 && Stat(name) < value; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Stat(name);
  };
  auto Set = [&](const String& new_preprocessed, const String& new_object) {
    std::lock_guard<Mutex> lock(mutex);
    preprocessed = new_preprocessed;
    object = new_object;
  };
  auto Output = [&] {
    Immutable content;
    EXPECT_TRUE(base::File::Read(output_path, &content));
    return content.string_copy();
  };

  // The new source isn't preprocessed: nothing can be found.
  Set("preprocessed 1", "object 1");
  Compile(1);
  EXPECT_EQ("object 1", Output());
  EXPECT_EQ(0u, Stat(perf::proto::Metric::PARALLEL_PREPROCESSING));

  // The compilation ends after the preprocessing: the result is stored for
  // both caches.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 2"_l));
  WaitAfterEdit();
  Set("preprocessed 2", "object 2");
  wait_for_preprocessing = true;
  Compile(2);
  EXPECT_EQ("object 2", Output());
  EXPECT_EQ(2u, compilations);
  // The next lookups of the same source wait for the stores.

  // The changed header gives the same preprocessed source: it's found before
  // the compilation ends. The edited sources aren't preprocessed at all.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 2 "_l));
  WaitAfterEdit();
  wait_for_preprocessing = false;
  wait_for_cancel = true;
  Set("preprocessed 2", "object 3");
  struct utimbuf old_time = {time(nullptr) - 100, time(nullptr) - 100};
  ASSERT_EQ(0, utime(output_path.c_str(), &old_time));
  Compile(3);
  EXPECT_EQ("object 2", Output());
  EXPECT_EQ(1u, WaitForStat(perf::proto::Metric::PARALLEL_PREPROCESSING_HIT, 1u));
  EXPECT_EQ(3u, compilations);
  EXPECT_TRUE(WaitFor([&] { return canceled == 1; }));
  // The output is the same - it keeps its time.
  struct stat st;
  ASSERT_EQ(0, stat(output_path.c_str(), &st));
  EXPECT_EQ(old_time.modtime, st.st_mtime);

  // The compilation ends before the preprocessing - and a new one is stored
  // for the simple cache too.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 4"_l));
  WaitAfterEdit();
  wait_for_cancel = false;
  Set("preprocessed 4", "object 4");
  Compile(4);
  EXPECT_EQ("object 4", Output());
  // The preprocessing may end after the reply - and store the result then:
  // the miss is counted after the store.
  EXPECT_EQ(2u, WaitForStat(perf::proto::Metric::SIMPLE_CACHE_MISS, 2u));

  ASSERT_TRUE(base::File::Write(header_path, "#define A 4 "_l));
  WaitAfterEdit();
  wait_for_cancel = true;
  Set("preprocessed 4", "object 5");
  Compile(5);
  EXPECT_EQ("object 4", Output());
  EXPECT_EQ(2u, WaitForStat(perf::proto::Metric::PARALLEL_PREPROCESSING_HIT, 2u));
  EXPECT_EQ(4u, Stat(perf::proto::Metric::PARALLEL_PREPROCESSING));

  emitter.reset();
}

TEST_F(EmitterTest, PreprocessAfterReply) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A 1"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::WHEN_IDLE);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  // The real socket of the client: the processes may be canceled, when it's
  // closed - and it's closed after the reply.
  int fds[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->SetNativeHandle(fds[0]);
    connection->CallOnSend([&](const net::Connection::Message& message) {
      send_condition.notify_all();
    });
    return true;
  };

  auto WaitFor = [](const Fn<bool()>& condition) {
    for (int i = 0; i < 300 && !condition(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
  };
  Mutex mutex;
  String preprocessed, object;
  // The preprocessing ends after this reply - or is canceled.
  Atomic<ui32> preprocessing_until_reply = 0;
  bool cancel_compilation = false, wait_for_cancel = false;
  Atomic<bool> preprocessing_started = false, preprocessing_canceled = false;
  Atomic<int> preprocessing_cancel_fd = -2, compilation_cancel_fd = -2;
  const auto deps = Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                    Immutable(header_path);
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      preprocessing_cancel_fd = process->cancel_fd_;
      preprocessing_started = true;
      EXPECT_TRUE(WaitFor([&] {
        return send_count >= preprocessing_until_reply ||
               process->cancel_requested();
      }));
      preprocessing_canceled = bool(process->cancel_requested());
      std::lock_guard<Mutex> lock(mutex);
      process->stdout_ = Immutable(String(preprocessed));
      WriteDepsFile(process, deps);
      return;
    }

    compilation_cancel_fd = process->cancel_fd_;
    if (cancel_compilation) {
      // The client is gone.
      EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_started); }));
      process->Cancel();
      return;
    }
    if (wait_for_cancel) {
      // Canceled by the hit of the simple cache.
      EXPECT_TRUE(WaitFor([&] { return process->cancel_requested(); }));
      return;
    }
    std::lock_guard<Mutex> lock(mutex);
    EXPECT_TRUE(base::File::Write(output_path, Immutable(String(object))));
    EXPECT_TRUE(base::File::Write(deps_path, deps));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count,
                     std::chrono::milliseconds timeout = Seconds(5)) {
    preprocessing_started = false;
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    return send_condition.wait_for(lock, timeout, [&] {
      return send_count == expected_send_count;
    });
  };

  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };

  // The new source isn't preprocessed.
  preprocessed = "preprocessed 1";
  object = "object 1";
  ASSERT_TRUE(Compile(1));

  // The preprocessing ends after the reply: it isn't canceled by the closed
  // socket, and stores the result for the simple cache.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 2"_l));
  WaitAfterEdit();
  preprocessed = "preprocessed 2";
  object = "object 2";
  preprocessing_until_reply = 2;
  ASSERT_TRUE(Compile(2));
  EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_started); }));
  EXPECT_EQ(fds[0], compilation_cancel_fd);
  EXPECT_EQ(-1, preprocessing_cancel_fd);
  EXPECT_TRUE(WaitFor(
      [&] { return Stat(perf::proto::Metric::SIMPLE_CACHE_MISS) >= 1; }));
  EXPECT_FALSE(preprocessing_canceled);

  // The same preprocessed source is found.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 2 "_l));
  WaitAfterEdit();
  object = "object 3";
  preprocessing_until_reply = 0;
  wait_for_cancel = true;
  ASSERT_TRUE(Compile(3));
  EXPECT_TRUE(WaitFor([&] {
    return Stat(perf::proto::Metric::PARALLEL_PREPROCESSING_HIT) >= 1;
  }));

  // The compilation is canceled: the preprocessing is useless, and canceled
  // too.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 4"_l));
  WaitAfterEdit();
  preprocessed = "preprocessed 4";
  preprocessing_until_reply = 100;
  wait_for_cancel = false;
  cancel_compilation = true;
  EXPECT_FALSE(Compile(4, std::chrono::milliseconds(500)));
  EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_canceled); }));

  emitter.reset();
  close(fds[0]);
  close(fds[1]);
}

TEST_F(EmitterTest, PreprocessWhenIdleWithEditedHeader) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "header.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));
  ASSERT_TRUE(base::File::Write(header_path, "#define A 1"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::WHEN_IDLE);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  Atomic<bool> replied = false;
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      const auto& status = message.status();
      EXPECT_EQ(net::proto::Status::OK, status.code()) << status.description();
      replied = true;
      send_condition.notify_all();
    });
    return true;
  };

  auto WaitFor = [](const Fn<bool()>& condition) {
    for (int i = 0; i < 300 && !condition(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
  };

  // The preprocessed source and the object, that the compiler gives.
  Mutex mutex;
  String preprocessed, object;
  bool edit_after_reply = false, check_cancel = false;
  Atomic<bool> preprocessing_done = false;
  Atomic<ui32> canceled = 0;
  const auto deps = Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                    Immutable(header_path);
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      if (edit_after_reply) {
        // The header is saved after the compilation is stored - and the
        // preprocessing reads the new version.
        EXPECT_TRUE(WaitFor([&] { return bool(replied); }));
        EXPECT_TRUE(base::File::Write(header_path, "#define A 3"_l));
      }
      std::lock_guard<Mutex> lock(mutex);
      process->stdout_ = Immutable(String(preprocessed));
      WriteDepsFile(process, deps);
      preprocessing_done = true;
      return;
    }

    if (check_cancel) {
      // The preprocessed source is looked up in the meantime.
      EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_done); }));
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (process->cancel_requested()) {
        ++canceled;
        return;
      }
    }
    std::lock_guard<Mutex> lock(mutex);
    EXPECT_TRUE(base::File::Write(output_path, Immutable(String(object))));
    EXPECT_TRUE(base::File::Write(deps_path, deps));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    preprocessing_done = false;
    replied = false;
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
  };

  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };
  auto Output = [&] {
    Immutable content;
    EXPECT_TRUE(base::File::Read(output_path, &content));
    return content.string_copy();
  };

  // The new source isn't preprocessed.
  preprocessed = "preprocessed 1";
  object = "object 1";
  Compile(1);
  EXPECT_EQ("object 1", Output());

  // The header is saved again during the preprocessing: the preprocessed
  // source doesn't match the object.
  ASSERT_TRUE(base::File::Write(header_path, "#define A 2"_l));
  WaitAfterEdit();
  preprocessed = "preprocessed 3";
  object = "object 2";
  edit_after_reply = true;
  Compile(2);
  EXPECT_EQ("object 2", Output());
  EXPECT_TRUE(WaitFor([&] { return bool(preprocessing_done); }));
  // The preprocessing ends after the reply.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(1u, Stat(perf::proto::Metric::PARALLEL_PREPROCESSING));

  // The new version of the header isn't found with the old object.
  WaitAfterEdit();
  edit_after_reply = false;
  check_cancel = true;
  object = "object 3";
  Compile(3);
  EXPECT_EQ("object 3", Output());
  EXPECT_EQ(0u, canceled);
  EXPECT_EQ(0u, Stat(perf::proto::Metric::PARALLEL_PREPROCESSING_HIT));
  EXPECT_EQ(2u, Stat(perf::proto::Metric::PARALLEL_PREPROCESSING));

  emitter.reset();
}

TEST_F(EmitterTest, TagEntries) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(3600);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  Mutex replies_mutex;
  List<net::Connection::Message> replies;
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      {
        std::lock_guard<Mutex> lock(replies_mutex);
        replies.emplace_back();
        replies.back().CopyFrom(message);
      }
      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      process->stdout_ = "preprocessed"_l;
      return;
    }
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Send = [&](net::Connection::ScopedMessage message,
                  ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
    std::lock_guard<Mutex> replies_lock(replies_mutex);
    return replies.back();
  };
  auto Compile = [&](const String& tags, ui32 expected_send_count) {
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    if (!tags.empty()) {
      extension->add_env("DC_TAGS=" + tags);
    }
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    Send(std::move(message), expected_send_count);
  };
  auto Request = [&](const String& forget, bool dry_run,
                     ui32 expected_send_count) {
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* request =
        message->mutable_tags_request();
    if (!forget.empty()) {
      request->set_forget(forget);
      request->set_dry_run(dry_run);
    }
    auto reply = Send(std::move(message), expected_send_count);
    EXPECT_TRUE(reply.has_tags_report());
    return reply.tags_report();
  };

  // Stored on one branch, and found on another one.
  Compile("branch:a", 1);
  // Any whitespace separates the tags.
  Compile("branch:b \tworktree:1\n", 2);

  auto report = Request(String(), true, 3);
  ASSERT_EQ(3, report.tags_size());
  EXPECT_EQ("branch:a", report.tags(0).name());
  EXPECT_EQ("branch:b", report.tags(1).name());
  EXPECT_EQ("worktree:1", report.tags(2).name());
  for (const auto& tag : report.tags()) {
    EXPECT_EQ(1u, tag.entries());
    EXPECT_EQ(0u, tag.exclusive_entries());
    EXPECT_LT(0u, tag.size());
  }
  EXPECT_EQ(3, report.overlaps_size());

  // The entry is used by other tags.
  report = Request("branch:a", false, 4);
  EXPECT_EQ(0u, report.removed_entries());
  EXPECT_EQ(1u, report.shared_entries());
  report = Request(String(), true, 5);
  EXPECT_EQ(2, report.tags_size());

  emitter.reset();
}

TEST_F(EmitterTest, DaemonStatus) {
  const base::TemporaryDir temp_dir;
  const String cache_path = temp_dir.path() / "cache";
  Mutex replies_mutex;
  List<net::Connection::Message> replies;
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      {
        std::lock_guard<Mutex> lock(replies_mutex);
        replies.emplace_back();
        replies.back().CopyFrom(message);
      }
      send_condition.notify_all();
    });
    return true;
  };

  // The state, that "dist-clang --stats" shows.
  auto Status = [&](const String& config_path, const String& config_error) {
    emitter = std::make_unique<Emitter>(conf);
    emitter->SetConfigStatus(config_path, config_error);
    EXPECT_TRUE(emitter->Initialize());
    const ui32 sends = send_count;
    net::Connection::ScopedMessage message(new net::Connection::Message);
    message->mutable_report();
    auto connection = test_service->TriggerListen(socket_path);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(std::static_pointer_cast<net::TestConnection>(connection)
                    ->TriggerReadAsync(std::move(message), status));
    {
      UniqueLock lock(send_mutex);
      EXPECT_TRUE(send_condition.wait_for(
          lock, Seconds(5), [&] { return send_count == sends + 1; }));
    }
    emitter.reset();
    // The next daemon has its own service.
    test_service = nullptr;
    std::lock_guard<Mutex> lock(replies_mutex);
    EXPECT_TRUE(replies.back().has_report());
    return replies.back().report().daemon();
  };

  // The broken config, and no cache.
  auto daemon = Status("/conf/daemon.conf", "line 1: broken");
  EXPECT_FALSE(daemon.version().empty());
  EXPECT_EQ("/conf/daemon.conf", daemon.config_path());
  EXPECT_EQ("line 1: broken", daemon.config_error());
  EXPECT_TRUE(daemon.cache_path().empty());
  EXPECT_NE(String::npos, daemon.cache_error().find("not configured"))
      << daemon.cache_error();

  conf.mutable_cache()->set_path(cache_path);
  conf.mutable_cache()->set_disabled(true);
  daemon = Status(String(), String());
  EXPECT_TRUE(daemon.config_path().empty());
  EXPECT_TRUE(daemon.config_error().empty());
  EXPECT_TRUE(daemon.cache_path().empty());
  EXPECT_NE(String::npos, daemon.cache_error().find("disabled"))
      << daemon.cache_error();

  // The working cache.
  conf.mutable_cache()->set_disabled(false);
  daemon = Status(String(), String());
  EXPECT_EQ(cache_path, daemon.cache_path());
  EXPECT_TRUE(daemon.cache_error().empty()) << daemon.cache_error();

  // The cache of another daemon.
  {
    cache::FileCache other(cache_path);
    ASSERT_TRUE(other.Run(3600));
    daemon = Status(String(), String());
    EXPECT_TRUE(daemon.cache_path().empty());
    EXPECT_NE(String::npos, daemon.cache_error().find("another daemon"))
        << daemon.cache_error();
  }
}

TEST_F(EmitterTest, Stop) {
  Mutex replies_mutex;
  List<net::Connection::Message> replies;
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      {
        std::lock_guard<Mutex> lock(replies_mutex);
        replies.emplace_back();
        replies.back().CopyFrom(message);
      }
      send_condition.notify_all();
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  Atomic<ui32> stops = {0};
  emitter->SetStopCallback([&] { ++stops; });
  ASSERT_TRUE(emitter->Initialize());

  // The daemon replies, and then stops - like with SIGTERM.
  net::Connection::ScopedMessage message(new net::Connection::Message);
  message->mutable_stop_request();
  auto connection = test_service->TriggerListen(socket_path);
  net::proto::Status status;
  status.set_code(net::proto::Status::OK);
  EXPECT_TRUE(std::static_pointer_cast<net::TestConnection>(connection)
                  ->TriggerReadAsync(std::move(message), status));
  {
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5),
                                        [&] { return send_count == 1u; }));
  }
  {
    std::lock_guard<Mutex> lock(replies_mutex);
    ASSERT_EQ(1u, replies.size());
    ASSERT_TRUE(replies.back().has_status());
    EXPECT_EQ(net::proto::Status::OK, replies.back().status().code());
  }
  EXPECT_EQ(1u, stops);

  emitter.reset();
}

TEST_F(EmitterTest, IsolatedTags) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(3600);
  auto* tag = conf.mutable_cache()->add_tags();
  tag->set_name("bench");
  tag->set_isolated(true);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  Atomic<ui32> compilations = {0};
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      process->stdout_ = "preprocessed"_l;
      return;
    }
    ++compilations;
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](const String& tags, ui32 expected_send_count) {
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    extension->add_env("DC_TAGS=" + tags);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");

    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The build of the isolated tag doesn't find the entry of others - and
  // finds its own one. Others find it too.
  Compile("other", 1);
  EXPECT_EQ(1u, compilations);
  Compile("bench", 2);
  EXPECT_EQ(2u, compilations);
  Compile("bench", 3);
  EXPECT_EQ(2u, compilations);
  Compile("other\nbench", 4);
  EXPECT_EQ(2u, compilations);
  Compile("other", 5);
  EXPECT_EQ(2u, compilations);

  emitter.reset();
}

TEST_F(EmitterTest, LimitLinksOfOutputs) {
  {
    // The copy-on-write clones are preferred to the hard links.
    const base::TemporaryDir temp_dir;
    base::File::CloneMode mode;
    ASSERT_TRUE(base::File::Write(temp_dir.path() / "a", "a"_l));
    ASSERT_TRUE(base::File::Clone(temp_dir.path() / "a", temp_dir.path() / "b",
                                  true, &mode));
    if (mode == base::File::CloneMode::REFLINK) {
      GTEST_SKIP() << "The file system clones the files";
    }
  }

  for (const ui32 max_links : {1u, 0u}) {
    const base::TemporaryDir temp_dir;
    const auto input_path = temp_dir.path() / "test.cc";
    const auto output1_path = temp_dir.path() / "test1.o";
    const auto output2_path = temp_dir.path() / "test2.o";
    const auto deps_path = temp_dir.path() / "test.d";
    const String compiler_version = "fake_compiler_version";

    ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

    conf.Clear();
    conf.mutable_emitter()->set_socket_path(socket_path);
    conf.mutable_emitter()->set_preprocessing(
        proto::Configuration::Emitter::NEVER);
    conf.mutable_cache()->set_path(temp_dir.path() / "cache");
    conf.mutable_cache()->set_direct(true);
    conf.mutable_cache()->set_link_outputs(true);
    conf.mutable_cache()->set_max_output_links(max_links);
    auto* version = conf.add_versions();
    version->set_version(compiler_version);
    version->set_path("fake_compiler_path");

    String output_path;
    run_callback = [&](base::TestProcess* process) {
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
      EXPECT_TRUE(base::File::Write(
          deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
    };

    send_count = 0;
    emitter = std::make_unique<Emitter>(conf);
    ASSERT_TRUE(emitter->Initialize());

    auto Compile = [&](const Path& output, ui32 expected_send_count) {
      output_path = output;
      auto connection = test_service->TriggerListen(socket_path);
      SharedPtr<net::TestConnection> test_connection =
          std::static_pointer_cast<net::TestConnection>(connection);
      net::Connection::ScopedMessage message(new net::Connection::Message);
      auto* extension =
          message->mutable_local();
      extension->set_current_dir(temp_dir);
      auto* flags = extension->mutable_flags();
      flags->set_input(input_path);
      flags->set_output(output);
      flags->set_deps_file(deps_path);
      flags->set_deps_target("test.o");
      flags->mutable_compiler()->set_version(compiler_version);
      flags->set_action("fake_action");
      net::proto::Status status;
      status.set_code(net::proto::Status::OK);
      EXPECT_TRUE(
          test_connection->TriggerReadAsync(std::move(message), status));
      UniqueLock lock(send_mutex);
      EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
        return send_count == expected_send_count;
      }));
    };
    auto Links = [](const Path& path) {
      struct stat st;
      EXPECT_EQ(0, stat(path.c_str(), &st));
      return st.st_nlink;
    };

    // The output is linked with the cache.
    Compile(output1_path, 1);
    EXPECT_EQ(2u, Links(output1_path));

    // Another build gets a copy - or a link, if unlimited.
    Compile(output2_path, 2);
    EXPECT_EQ(max_links ? 1u : 3u, Links(output2_path));
    EXPECT_EQ(max_links ? 2u : 3u, Links(output1_path));

    // The first output is gone - the cache file is linked again.
    ASSERT_TRUE(base::File::Delete(output1_path));
    Compile(output2_path, 3);
    EXPECT_EQ(2u, Links(output2_path));

    // Restoring the linked output again keeps the link.
    Compile(output2_path, 4);
    EXPECT_EQ(2u, Links(output2_path));

    emitter.reset();
    test_service = nullptr;
  }
}

TEST_F(EmitterTest, RestoreBlobCompressedAfterLookup) {
  // The blob of the found entry is compressed by the cleaner, before the
  // output is restored - e.g. during the check of the warnings.
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const auto cache_path = temp_dir.path() / "cache";
  const String compiler_version = "fake_compiler_version";
  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::NEVER);
  conf.mutable_cache()->set_path(cache_path);
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_link_outputs(true);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  const auto object = "object"_l;
  const cache::BlobStore blobs(cache_path / "blobs", cache::proto::ZSTD, true);
  const String object_hash = base::Hexify(Immutable(object).Hash());
  List<String> runs;
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(),
                  "-fsyntax-only"_l) != process->args_.end()) {
      runs.push_back("check");
      String packed;
      EXPECT_TRUE(cache::Pack(cache::proto::ZSTD, object, &packed));
      EXPECT_TRUE(base::File::Write(blobs.PackedPath(object_hash),
                                    Immutable(std::move(packed))));
      EXPECT_TRUE(base::File::Delete(blobs.RawPath(object_hash)));
      return;
    }
    runs.push_back("compile");
    EXPECT_TRUE(base::File::Write(output_path, object));
    EXPECT_TRUE(base::File::Write(
        deps_path, Immutable("test.o: "_l) + Immutable(input_path)));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count, const char* warning) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->set_recheck_warnings(true);
    flags->add_warnings()->add_values(warning);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
  };

  Compile(1, "-Wall");
  EXPECT_EQ((List<String>{"compile"}), runs);

  // Other warnings - only the check, and the output from the cache.
  ASSERT_TRUE(base::File::Delete(output_path));
  runs.clear();
  Compile(2, "-Wextra");
  EXPECT_EQ((List<String>{"check"}), runs);
  Immutable content;
  ASSERT_TRUE(base::File::Read(output_path, &content));
  EXPECT_EQ(object, content);

  emitter.reset();
}

TEST_F(EmitterTest, RecheckWarnings) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";
  const String compiler_path = "fake_compiler_path";

  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path(compiler_path);

  // The sends are counted before the callback: the description is taken,
  // when it's written.
  String last_description;
  ui32 descriptions = 0;
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();
      {
        UniqueLock lock(send_mutex);
        last_description = status.description();
        ++descriptions;
      }

      send_condition.notify_all();
    });
    return true;
  };

  List<String> runs;
  bool fail_check = false;
  run_callback = [&](base::TestProcess* process) {
    auto Has = [&](Literal arg) {
      return std::find(process->args_.begin(), process->args_.end(),
                       Immutable(arg)) != process->args_.end();
    };
    do_run = !fail_check || !Has("-fsyntax-only"_l);
    const auto deps = Immutable("test.o: "_l) + Immutable(input_path);
    if (Has("-E"_l)) {
      runs.push_back("-E");
      process->stdout_ = "preprocessed"_l;
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    } else if (Has("-fsyntax-only"_l)) {
      runs.push_back("-fsyntax-only");
      EXPECT_TRUE(Has("-Wextra"_l) || Has("-fdiagnostics-format=msvc"_l) ||
                  fail_check);
      EXPECT_FALSE(Has("-o"_l));
      process->stderr_ = Has("-fdiagnostics-format=msvc"_l)
                             ? "msvc warning"_l
                             : "new warning"_l;
    } else {
      runs.push_back("compile");
      process->stderr_ = "old warning"_l;
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count, const List<String>& warnings,
                     const String& format = String()) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->set_recheck_warnings(true);
    for (const auto& warning : warnings) {
      flags->add_warnings()->add_values(warning);
    }
    if (!format.empty()) {
      flags->add_non_direct()->add_values(format);
    }
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return descriptions == expected_send_count;
    }));
  };

  Compile(1, {"-Wall"});
  EXPECT_EQ((List<String>{"-E", "compile"}), runs);
  EXPECT_EQ("old warning", last_description);

  // The same warnings - the cached diagnostics.
  ASSERT_TRUE(base::File::Delete(output_path));
  runs.clear();
  Compile(2, {"-Wall"});
  EXPECT_TRUE(runs.empty());
  EXPECT_EQ("old warning", last_description);

  // Other warnings - only the check.
  ASSERT_TRUE(base::File::Delete(output_path));
  runs.clear();
  Compile(3, {"-Wall", "-Wextra"});
  EXPECT_EQ((List<String>{"-fsyntax-only"}), runs);
  EXPECT_EQ("new warning", last_description);

  // The same warnings in another format - the check too.
  ASSERT_TRUE(base::File::Delete(output_path));
  runs.clear();
  Compile(4, {"-Wall"}, "-fdiagnostics-format=msvc");
  EXPECT_EQ((List<String>{"-fsyntax-only"}), runs);
  EXPECT_EQ("msvc warning", last_description);

  Immutable object;
  EXPECT_TRUE(base::File::Read(output_path, &object));
  EXPECT_EQ("object"_l, object);

  // The check fails - e.g. with "-Werror": it isn't a hit.
  ResetStatistics();
  fail_check = true;
  expected_code = net::proto::Status::EXECUTION;
  runs.clear();
  Compile(5, {"-Wall", "-Wunused"});
  EXPECT_EQ((List<String>{"-fsyntax-only"}), runs);
  for (const auto name : {perf::proto::Metric::DIRECT_CACHE_HIT,
                          perf::proto::Metric::SIMPLE_CACHE_HIT,
                          perf::proto::Metric::TIME_SAVED_BY_DIRECT_HITS,
                          perf::proto::Metric::TIME_SAVED_BY_SIMPLE_HITS}) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    EXPECT_EQ(0u, metric.value()) << perf::proto::Metric::Name_Name(name);
  }

  emitter.reset();
}

TEST_F(EmitterTest, DontStoreIfInputsChangedDuringCompilation) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "test.h";
  const auto output_path = temp_dir.path() / "test.o";
  const auto deps_path = temp_dir.path() / "test.d";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "int a;"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      send_condition.notify_all();
    });
    return true;
  };

  ui32 compilations = 0;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    const auto deps = Immutable("test.o: "_l) + Immutable(input_path) + " "_l +
                      Immutable(header_path);
    if (preprocessing) {
      process->stdout_ = "preprocessed"_l;
    } else {
      ++compilations;
      // The header is edited during the first compilation.
      if (compilations == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_TRUE(base::File::Write(header_path, "int b;"_l));
      }
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
      EXPECT_TRUE(base::File::Write(deps_path, deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->set_deps_file(deps_path);
    flags->set_deps_target("test.o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The result isn't stored: it may be compiled from the old header.
  Compile(1);
  EXPECT_EQ(1u, compilations);

  // The same inputs - the compilation again.
  WaitAfterEdit();
  Compile(2);
  EXPECT_EQ(2u, compilations);

  // Now it's cached.
  Compile(3);
  EXPECT_EQ(2u, compilations);

  emitter.reset();
}

TEST_F(EmitterTest, DontStoreIfHeadersChangedDuringCompilationWithoutDeps) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header_path = temp_dir.path() / "test.h";
  const auto output_path = temp_dir.path() / "test.o";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "int a;"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_clean_period(1);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      send_condition.notify_all();
    });
    return true;
  };

  // Without the dependency file the headers are known only from the line
  // markers of the preprocessed source.
  ui32 compilations = 0;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    if (preprocessing) {
      const Immutable input(input_path), header(header_path);
      process->stdout_ = Immutable("# 1 \""_l) + input + "\"\n# 1 \""_l +
                         header + "\" 1\nint a;\n# 2 \""_l + input +
                         "\" 2\n"_l;
    } else {
      ++compilations;
      // The header is edited during the first compilation - after the
      // preprocessing.
      if (compilations == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_TRUE(base::File::Write(header_path, "int b;"_l));
      }
      EXPECT_TRUE(base::File::Write(output_path, "object"_l));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    }));
  };

  // The result isn't stored: it may be compiled from the new header, but
  // under the hash of the old one.
  Compile(1);
  EXPECT_EQ(1u, compilations);

  WaitAfterEdit();
  Compile(2);
  EXPECT_EQ(2u, compilations);

  // Now it's cached.
  Compile(3);
  EXPECT_EQ(2u, compilations);

  emitter.reset();
}

TEST_F(EmitterTest, ReplyToSimpleHitBeforeDirectStore) {
  const base::TemporaryDir temp_dir;
  const auto input1_path = temp_dir.path() / "test1.cc";
  const auto input2_path = temp_dir.path() / "test2.cc";
  const auto header_path = temp_dir.path() / "test.h";
  const String compiler_version = "fake_compiler_version";

  ASSERT_TRUE(base::File::Write(input1_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(input2_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "int a;"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::ALWAYS);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      send_condition.notify_all();
    });
    return true;
  };

  ui32 compilations = 0;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    const auto deps = Immutable("test.o: "_l) + Immutable(header_path);
    WriteDepsFile(process, deps);
    if (preprocessing) {
      process->stdout_ = "preprocessed"_l;
    } else {
      ++compilations;
      EXPECT_TRUE(
          base::File::Write(temp_dir.path() / "test1.o", "object"_l));
      EXPECT_TRUE(base::File::Write(temp_dir.path() / "test1.d", deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Compile = [&](const Path& input, ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    const auto name = input.stem().string();
    flags->set_input(input);
    flags->set_output(name + ".o");
    flags->set_deps_file(name + ".d");
    flags->set_deps_target(name + ".o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    return send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    });
  };

  EXPECT_TRUE(Compile(input1_path, 1));
  EXPECT_EQ(1u, compilations);
  // The direct store of the first source ends.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The header can't be read for the direct store of the other source, until
  // it's written.
  ASSERT_TRUE(base::File::Delete(header_path));
  ASSERT_EQ(0, mkfifo(header_path.c_str(), 0600));
  WaitAfterEdit();

  // The simple hit doesn't wait for the direct store.
  EXPECT_TRUE(Compile(input2_path, 2));
  EXPECT_EQ(1u, compilations);

  {
    std::ofstream header(header_path);
    header << "int a;";
  }

  emitter.reset();
}

TEST_F(EmitterTest, TagSimpleHitBeforeReply) {
  // After the reply the client may ask for the stats of the tags at once.
  const base::TemporaryDir temp_dir;
  const auto input1_path = temp_dir.path() / "test1.cc";
  const auto input2_path = temp_dir.path() / "test2.cc";
  const auto header_path = temp_dir.path() / "test.h";
  const String compiler_version = "fake_compiler_version";
  Mutex replies_mutex;
  List<net::Connection::Message> replies;

  ASSERT_TRUE(base::File::Write(input1_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(input2_path, "#include \"test.h\""_l));
  ASSERT_TRUE(base::File::Write(header_path, "int a;"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  conf.mutable_cache()->set_clean_period(1);
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::ALWAYS);

  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      std::lock_guard<Mutex> lock(replies_mutex);
      replies.push_back(message);
      send_condition.notify_all();
    });
    return true;
  };

  ui32 compilations = 0;
  run_callback = [&](base::TestProcess* process) {
    const bool preprocessing =
        std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end();
    const auto deps = Immutable("test.o: "_l) + Immutable(header_path);
    WriteDepsFile(process, deps);
    if (preprocessing) {
      process->stdout_ = "preprocessed"_l;
    } else {
      ++compilations;
      EXPECT_TRUE(
          base::File::Write(temp_dir.path() / "test1.o", "object"_l));
      EXPECT_TRUE(base::File::Write(temp_dir.path() / "test1.d", deps));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto Send = [&](net::Connection::ScopedMessage message,
                  ui32 expected_send_count) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));

    UniqueLock lock(send_mutex);
    return send_condition.wait_for(lock, Seconds(1), [&] {
      return send_count == expected_send_count;
    });
  };
  auto Compile = [&](const Path& input, const String& tag,
                     ui32 expected_send_count) {
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    extension->add_env("DC_TAGS=" + tag);
    auto* flags = extension->mutable_flags();
    const auto name = input.stem().string();
    flags->set_input(input);
    flags->set_output(name + ".o");
    flags->set_deps_file(name + ".d");
    flags->set_deps_target(name + ".o");
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("-c");
    return Send(std::move(message), expected_send_count);
  };

  EXPECT_TRUE(Compile(input1_path, "a", 1));
  EXPECT_EQ(1u, compilations);
  // The direct store of the first source ends.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The header can't be read for the direct store of the other source, until
  // it's written.
  ASSERT_TRUE(base::File::Delete(header_path));
  ASSERT_EQ(0, mkfifo(header_path.c_str(), 0600));
  WaitAfterEdit();

  // The direct store of the simple hit waits for the header.
  EXPECT_TRUE(Compile(input2_path, "b", 2));
  EXPECT_EQ(1u, compilations);

  net::Connection::ScopedMessage request(new net::Connection::Message);
  request->mutable_tags_request();
  EXPECT_TRUE(Send(std::move(request), 3));
  base::proto::TagsReport report;
  {
    std::lock_guard<Mutex> lock(replies_mutex);
    report = replies.back().tags_report();
  }

  {
    std::ofstream header(header_path);
    header << "int a;";
  }

  ASSERT_EQ(2, report.tags_size());
  EXPECT_EQ("a", report.tags(0).name());
  EXPECT_EQ("b", report.tags(1).name());

  emitter.reset();
}

TEST_F(EmitterTest, StoreDirectCacheForLocalResult) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto input1_path = temp_dir.path() / "test1.cc";
  // The same source is compiled again - into another place.
  const auto input2_path = input1_path;
  const auto header1_path = temp_dir.path() / "header1.h";
  const auto header2_path = temp_dir.path() / "header2.h";
  const auto source_code = "int main() {}"_l;

  ASSERT_TRUE(base::File::Write(input1_path, source_code));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

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
  const auto deps1_path = "test1.d"_l;
  const auto deps2_path = temp_dir.path() / "test2.d";
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output1_path = "test1.o"_l;
  const auto object_code = "fake_object_code"_l;
  const auto deps_contents = "test1.o: test1.cc header1.h header2.h"_l;

  const auto output2_path = temp_dir.path() / "test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ(
          (Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input1_path}),
          WithoutDepsFile(process));
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps1_path, deps_contents));
      WriteDepsFile(process, deps_contents);
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language,
                                 "-o"_l, output1_path, input1_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output1_path, object_code));
      // The hit saves this time.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  emitter.reset();

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());
  metric.set_name(perf::proto::Metric::TIME_SAVED_BY_DIRECT_HITS);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_LE(100u, metric.value());

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
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

TEST_F(EmitterTest, StoreDirectCacheForLocalResultWithAndWithoutIncludedHeaders) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto header1_path = temp_dir.path() / "header1.h";
  const auto header2_path = temp_dir.path() / "header2.h";
  const auto source_code = "int main() {}"_l;
  const auto deps_path = "test.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output_path = "test.o"_l;
  const auto object_code = "fake_object_code"_l;
  const auto preprocessed_header_path = temp_dir.path() / "header1.h.pth";
  const auto deps_contents = "test.o: test.cc header1.h header2.h"_l;

  // Clang outputs headers included using "-include-pth/pch" as absolute paths.
  const auto preprocessed_deps_contents = Immutable("test.o: test.cc header1.h header2.h "_l) + header1_path;
  const auto preprocessed_contents = "Any content should work"_l;

  ASSERT_TRUE(base::File::Write(preprocessed_header_path, preprocessed_contents));

  ASSERT_TRUE(base::File::Write(input_path, source_code));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}),
                WithoutDepsFile(process));
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path, deps_contents));
      WriteDepsFile(process, deps_contents);
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language,
                                 "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path, object_code));
    } else if (run_count == 3) {
      EXPECT_EQ((Immutable::Rope{"-include-pth"_l, preprocessed_header_path, "-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}),
                WithoutDepsFile(process))
          << process->PrintArgs();
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path, preprocessed_deps_contents));
      WriteDepsFile(process, preprocessed_deps_contents);
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);

    auto* new_arg = extension->mutable_flags()->add_non_cached();
    new_arg->add_values("-include-pth");
    new_arg->add_values(preprocessed_header_path);

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  auto connection3 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection3);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);

    auto* new_arg = extension->mutable_flags()->add_non_cached();
    new_arg->add_values("-include-pth");
    new_arg->add_values(preprocessed_header_path);

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 3; }));
  }

  emitter.reset();

  EXPECT_EQ(3u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(3u, connect_count);
  EXPECT_EQ(3u, connections_created);
  EXPECT_EQ(3u, read_count);
  EXPECT_EQ(3u, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection3.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, StoreDirectCacheForRemoteResult) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto input1_path = temp_dir.path() / "test1.cc";
  // The same source is compiled again - into another place.
  const auto input2_path = input1_path;
  const auto header1_path = temp_dir.path() / "header1.h";
  const auto header2_path = temp_dir.path() / "header2.h";
  const auto source_code = "int main() {}"_l;

  ASSERT_TRUE(base::File::Write(input1_path, source_code));
  ASSERT_TRUE(base::File::Write(header1_path, "#define A"_l));
  ASSERT_TRUE(base::File::Write(header2_path, "#define B"_l));

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
  const auto deps1_path = "test1.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output1_path = "test1.o"_l;
  const auto object_code = "fake_object_code"_l;

  const auto output2_path = temp_dir.path() / "test2.o";
  // |output_path2| checks that everything works fine with absolute paths.

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    if (connect_count == 1) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    } else if (connect_count == 2) {
      // Connection from local daemon to remote daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_remote());
        const auto& command = message.remote();
        EXPECT_EQ(preprocessed_source, command.source());

        send_condition.notify_all();
      });

      connection->CallOnRead([&](net::Connection::Message* message) {
        message->mutable_result()->set_obj(object_code);
      });
    } else if (connect_count == 3) {
      // Connection from client to local daemon.

      connection->CallOnSend([&](const net::Connection::Message& message) {
        EXPECT_TRUE(message.has_status());
        const auto& status = message.status();
        EXPECT_EQ(expected_code, status.code()) << status.description();

        send_condition.notify_all();
      });
    }
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input1_path}),
              WithoutDepsFile(process));
    process->stdout_ = preprocessed_source;
    EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps1_path, "test1.o: test1.cc header1.h header2.h"_l));
    WriteDepsFile(process, "test1.o: test1.cc header1.h header2.h"_l);
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 3; }));
  }

  emitter.reset();

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

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
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
}

TEST_F(EmitterTest, DISABLED_UpdateDirectCacheFromLocalCache) {
  // TODO: implement this test.
  //       - Check that direct cache gets updated, if there is direct cache
  //         miss, but there is hit in normal cache.
}

TEST_F(EmitterTest, KeepUnchangedOutputsOnRestat) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const String compiler_version = "fake_compiler_version";
  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_emitter()->set_preprocessing(
      proto::Configuration::Emitter::ALWAYS);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend(
        [&](const net::Connection::Message&) { send_condition.notify_all(); });
    return true;
  };

  Mutex mutex;
  String preprocessed;
  Atomic<ui32> compilations = 0;
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      std::lock_guard<Mutex> lock(mutex);
      // The inputs are known from the markers.
      process->stdout_ =
          Immutable("# 1 \""_l) + Immutable(input_path) + "\"\n"_l +
          Immutable(String(preprocessed));
      return;
    }
    ++compilations;
    EXPECT_TRUE(base::File::Write(output_path, "object"_l));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  // Ninja with "restat" sets DC_RESTAT=1 for the rules, that may keep the
  // outputs.
  auto Compile = [&](ui32 expected_send_count, bool restat) {
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    if (restat) {
      extension->add_env("DC_RESTAT=1");
    }
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5), [&] {
      return send_count == expected_send_count;
    }));
  };
  const time_t old_time = time(nullptr) - 100;
  auto MakeOld = [&] {
    struct utimbuf times = {old_time, old_time};
    ASSERT_EQ(0, utime(output_path.c_str(), &times));
  };
  auto IsOld = [&] {
    struct stat st;
    EXPECT_EQ(0, stat(output_path.c_str(), &st));
    return st.st_mtime == old_time;
  };

  {
    std::lock_guard<Mutex> lock(mutex);
    preprocessed = "preprocessed 1";
  }
  Compile(1, true);
  EXPECT_EQ(1u, compilations);

  // The same object from the cache.
  MakeOld();
  Compile(2, true);
  EXPECT_EQ(1u, compilations);
  EXPECT_TRUE(IsOld());

  // Without "restat" the output is always new.
  Compile(3, false);
  EXPECT_EQ(1u, compilations);
  EXPECT_FALSE(IsOld());

  // The same object from the compiler.
  {
    std::lock_guard<Mutex> lock(mutex);
    preprocessed = "preprocessed 2";
  }
  MakeOld();
  Compile(4, true);
  EXPECT_EQ(2u, compilations);
  EXPECT_TRUE(IsOld());

  emitter.reset();
}

TEST_F(EmitterTest, SkipTaskWithClosedConnection) {
  const base::TemporaryDir temp_dir;
  const auto input_path = temp_dir.path() / "test.cc";
  const auto output_path = temp_dir.path() / "test.o";
  const String compiler_version = "fake_compiler_version";
  ASSERT_TRUE(base::File::Write(input_path, "int main() { return 1; }"_l));

  conf.mutable_cache()->set_path(temp_dir.path() / "cache");
  conf.mutable_cache()->set_direct(true);
  auto* version = conf.add_versions();
  version->set_version(compiler_version);
  version->set_path("fake_compiler_path");

  // The real sockets of the clients: they are gone, when the other end is
  // closed.
  int fds[2] = {-1, -1};
  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->SetNativeHandle(fds[0]);
    connection->CallOnSend(
        [&](const net::Connection::Message&) { send_condition.notify_all(); });
    return true;
  };
  bool client_leaves = false;
  Atomic<int> cancel_fd = -2;
  run_callback = [&](base::TestProcess* process) {
    if (std::find(process->args_.begin(), process->args_.end(), "-E"_l) !=
        process->args_.end()) {
      process->stdout_ = "preprocessed"_l;
      return;
    }
    cancel_fd = process->cancel_fd_;
    if (client_leaves) {
      // The build is interrupted: the compiler is killed with the client.
      close(fds[1]);
      process->Cancel();
      return;
    }
    EXPECT_TRUE(base::File::Write(output_path, "new object"_l));
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  HashMap<int, ui64> stats;
  auto Stat = [&stats](perf::proto::Metric::Name name) {
    perf::proto::Metric metric;
    metric.set_name(name);
    base::Singleton<perf::StatService>::Get().Dump(metric);
    return stats[name] += metric.value();
  };
  auto WaitForStat = [&Stat](perf::proto::Metric::Name name, ui64 value) {
    for (int i = 0; i < 300 && Stat(name) < value; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return Stat(name);
  };
  auto Compile = [&] {
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    auto connection = test_service->TriggerListen(socket_path);
    SharedPtr<net::TestConnection> test_connection =
        std::static_pointer_cast<net::TestConnection>(connection);
    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir);
    auto* flags = extension->mutable_flags();
    flags->set_input(input_path);
    flags->set_output(output_path);
    flags->mutable_compiler()->set_version(compiler_version);
    flags->set_action("fake_action");
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  };

  // The client is gone before the task is taken: nothing is run.
  ASSERT_TRUE(base::File::Write(output_path, "old object"_l));
  Compile();
  close(fds[1]);
  EXPECT_EQ(1u, WaitForStat(perf::proto::Metric::TASK_CANCELED, 1u));
  EXPECT_EQ(0u, run_count);
  close(fds[0]);

  // The client is gone during the compilation: the compiler is killed with
  // it - before it writes the outputs, that the build may have produced
  // meanwhile. Nothing is stored.
  client_leaves = true;
  Compile();
  EXPECT_EQ(2u, WaitForStat(perf::proto::Metric::TASK_CANCELED, 2u));
  EXPECT_EQ(fds[0], cancel_fd);
  close(fds[0]);

  // The next compilation isn't found in the cache.
  client_leaves = false;
  const ui32 runs = run_count;
  Compile();
  {
    UniqueLock lock(send_mutex);
    EXPECT_TRUE(send_condition.wait_for(lock, Seconds(5),
                                        [&] { return send_count == 1; }));
  }
  emitter.reset();
  EXPECT_LT(runs, run_count);
  Immutable output;
  ASSERT_TRUE(base::File::Read(output_path, &output));
  EXPECT_EQ("new object"_l, output);
  // Only the last client gets the reply.
  EXPECT_EQ(1u, send_count);
  close(fds[0]);
  close(fds[1]);
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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code_no_version, status.code());
    });
    return true;
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code_ok, status.code());
    });
    return true;
  };
  run_callback = [&](base::TestProcess* process) {
    EXPECT_EQ(Path(compiler_path), process->exec_path_);
    EXPECT_EQ((Immutable::Rope{action}), process->args_) << process->PrintArgs();
  };

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(current_dir);
    extension->mutable_flags()->mutable_compiler()->set_version(bad_version);
    extension->mutable_flags()->set_action(action);

    net::proto::Status status;
    status.set_code(net::proto::Status::OK);

    EXPECT_TRUE(test_connection->TriggerReadAsync(std::move(message), status));
  }

  emitter.reset();

  EXPECT_EQ(1u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";
}

TEST_F(EmitterTest, HitDirectCacheFromTwoLocations) {
  // Prepare environment.
  const base::TemporaryDir temp_dir1, temp_dir2;
  const auto input_path = temp_dir1.path() / "test.cc";
  const auto header_path = temp_dir1.path() / "header.h";
  const auto preprocessed_header_path = temp_dir1.path() / "header.h.pth";
  const auto sanitize_blacklist_path = temp_dir1.path() / "asan-blacklist.txt";
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto preprocessed_contents = "Any content should work"_l;
  const auto sanitize_blacklist_contents = "fun:main"_l;

  ASSERT_TRUE(base::File::Write(input_path, source_code));
  ASSERT_TRUE(base::File::Write(header_path, header_contents));
  ASSERT_TRUE(base::File::Write(preprocessed_header_path, preprocessed_contents));
  ASSERT_TRUE(base::File::Write(sanitize_blacklist_path, sanitize_blacklist_contents));

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
  const auto deps_path = "test.d"_l;
  const auto language = "fake_language"_l;
  const auto preprocessed_source = "fake_source"_l;
  const auto action = "fake_action"_l;
  const auto output_path = "test.o"_l;
  const auto object_code = "fake_object_code"_l;

  // Clang outputs headers included using "-include-pth/pch" as absolute paths.
  const auto deps_contents = Immutable("test.o: test.cc header.h "_l) + header_path;

  connect_callback = [&](net::TestConnection* connection, net::EndPointPtr) {
    connection->CallOnSend([&](const net::Connection::Message& message) {
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-include-pth"_l, preprocessed_header_path, "-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}),
                WithoutDepsFile(process))
          << process->PrintArgs();
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path, deps_contents));
      WriteDepsFile(process, deps_contents);
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{"-include-pth"_l, preprocessed_header_path, action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path,
                                 "-x"_l, language,
                                 Immutable("-fsanitize-blacklist="_l) + sanitize_blacklist_path, "-o"_l, output_path,
                                 input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path, object_code));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir1);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);

    auto* new_arg = extension->mutable_flags()->add_non_cached();
    new_arg->add_values("-include-pth");
    new_arg->add_values(preprocessed_header_path);

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    const auto input_path = temp_dir2.path() / "test.cc";
    const auto header_path = temp_dir2.path() / "header.h";
    const auto sanitize_blacklist_path = temp_dir2.path() / "asan-blacklist.txt";

    ASSERT_TRUE(base::File::Write(input_path, source_code));
    ASSERT_TRUE(base::File::Write(header_path, header_contents));
    ASSERT_TRUE(base::File::Write(sanitize_blacklist_path, sanitize_blacklist_contents));

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
    extension->set_current_dir(temp_dir2);

    extension->mutable_flags()->set_input(input_path);
    extension->mutable_flags()->set_output(output_path);
    extension->mutable_flags()->add_included_files(preprocessed_header_path);

    auto* new_arg = extension->mutable_flags()->add_non_cached();
    new_arg->add_values("-include-pth");
    new_arg->add_values(preprocessed_header_path);

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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  emitter.reset();

  perf::proto::Metric metric;
  metric.set_name(perf::proto::Metric::DIRECT_CACHE_HIT);
  base::Singleton<perf::StatService>::Get().Dump(metric);
  EXPECT_EQ(1u, metric.value());

  Immutable cache_output;
  const auto output2_path = temp_dir2.path() / output_path;
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = temp_dir2.path() / deps_path;
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

  EXPECT_EQ(2u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

TEST_F(EmitterTest, DontHitDirectCacheFromTwoRelativeSources) {
  // Prepare environment.
  const base::TemporaryDir temp_dir;
  const auto relpath = temp_dir.path() / "path1";
  ASSERT_TRUE(base::CreateDirectory(relpath));

  const auto input_path = relpath / "test.cc";
  const auto header_path = relpath / "header.h";
  const auto source_code = "int main() {}"_l;
  const auto header_contents = "#define A"_l;
  const auto header_contents2 = "#define B"_l;

  const auto relpath2 = temp_dir.path() / "path2";
  ASSERT_TRUE(base::CreateDirectory(relpath2));

  const auto input_path2 = relpath2 / "test.cc";
  const auto header_path2 = relpath2 / "header.h";

  ASSERT_TRUE(base::File::Write(input_path, source_code));
  ASSERT_TRUE(base::File::Write(header_path, header_contents));

  ASSERT_TRUE(base::File::Write(input_path2, source_code));
  ASSERT_TRUE(base::File::Write(header_path2, header_contents2));

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
      EXPECT_TRUE(message.has_status());
      const auto& status = message.status();
      EXPECT_EQ(expected_code, status.code()) << status.description();

      send_condition.notify_all();
    });
    return true;
  };

  run_callback = [&](base::TestProcess* process) {
    if (run_count == 1) {
      EXPECT_EQ((Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path}),
                WithoutDepsFile(process));
      process->stdout_ = preprocessed_source;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path, deps_contents));
      WriteDepsFile(process, deps_contents);
    } else if (run_count == 2) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language,
                                 "-o"_l, output_path, input_path}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path, object_code));
    } else if (run_count == 3) {
      EXPECT_EQ(
          (Immutable::Rope{"-E"_l, "-x"_l, language, "-o"_l, "-"_l, input_path2}),
          WithoutDepsFile(process));
      process->stdout_ = preprocessed_source2;
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / deps_path2, deps_contents2));
      WriteDepsFile(process, deps_contents2);
    } else if (run_count == 4) {
      EXPECT_EQ((Immutable::Rope{action, "-Xclang"_l, "-load"_l, "-Xclang"_l, plugin_path, "-x"_l, language,
                                 "-o"_l, output_path2, input_path2}),
                process->args_)
          << process->PrintArgs();
      EXPECT_TRUE(base::File::Write(process->cwd_path_ / output_path2, object_code2));
    }
  };

  emitter = std::make_unique<Emitter>(conf);
  ASSERT_TRUE(emitter->Initialize());

  auto connection1 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection1);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 1; }));
  }

  auto connection2 = test_service->TriggerListen(socket_path);
  {
    SharedPtr<net::TestConnection> test_connection = std::static_pointer_cast<net::TestConnection>(connection2);

    net::Connection::ScopedMessage message(new net::Connection::Message);
    auto* extension = message->mutable_local();
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
    EXPECT_TRUE(send_condition.wait_for(lock, std::chrono::seconds(1), [this] { return send_count == 2; }));
  }

  emitter.reset();

  Immutable cache_output;
  const auto output2_path = temp_dir.path() / output_path;
  EXPECT_TRUE(base::File::Exists(output2_path));
  EXPECT_TRUE(base::File::Read(output2_path, &cache_output));
  EXPECT_EQ(object_code, cache_output);

  Immutable cache_deps;
  const auto deps2_path = temp_dir.path() / deps_path;
  EXPECT_TRUE(base::File::Exists(deps2_path));
  EXPECT_TRUE(base::File::Read(deps2_path, &cache_deps));
  EXPECT_EQ(deps_contents, cache_deps);

  Immutable cache_output2;
  const auto output2_path2 = temp_dir.path() / output_path2;
  EXPECT_TRUE(base::File::Exists(output2_path2));
  EXPECT_TRUE(base::File::Read(output2_path2, &cache_output2));
  EXPECT_EQ(object_code2, cache_output2);

  Immutable cache_deps2;
  const auto deps2_path2 = temp_dir.path() / deps_path2;
  EXPECT_TRUE(base::File::Exists(deps2_path2));
  EXPECT_TRUE(base::File::Read(deps2_path2, &cache_deps2));
  EXPECT_EQ(deps_contents2, cache_deps2);

  EXPECT_EQ(4u, run_count);
  EXPECT_EQ(1u, listen_count);
  EXPECT_EQ(2u, connect_count);
  EXPECT_EQ(2u, connections_created);
  EXPECT_EQ(2u, read_count);
  EXPECT_EQ(2u, send_count);
  EXPECT_EQ(1, connection1.use_count()) << "Daemon must not store references to the connection";
  EXPECT_EQ(1, connection2.use_count()) << "Daemon must not store references to the connection";

  // TODO: check that original files are not moved.
  // TODO: check that removal of original files doesn't fail cache filling.
  // TODO: check situations about deps file:
  //       - deps file is in cache, but not requested.
}

TEST(EmitterSourceRootsTest, SetKeyRoot) {
  google::protobuf::RepeatedPtrField<String> roots;
  *roots.Add() = "/work/other";
  *roots.Add() = "/work/Tree/";

  auto KeyRoot = [&](const String& current_dir, const String& input,
                     const List<String>& remapped = {"../.."}) {
    base::proto::Flags flags;
    flags.set_input(input);
    for (const auto& prefix : remapped) {
      flags.add_remapped_prefixes(prefix);
    }
    Emitter::SetKeyRoot(roots, current_dir, &flags);
    return Pair<String>(flags.key_root(), flags.key_root_relative());
  };

  EXPECT_EQ((Pair<String>("/work/Tree", "../..")),
            KeyRoot("/work/Tree/out/Release", "../../src/a.cc"));
  EXPECT_EQ((Pair<String>("", "")), KeyRoot("/work", "tree2/a.cc"));

  // The root in another case is the same directory on Mac: the paths, that the
  // compiler sees, keep their spelling.
  if (base::CurrentPlatform() == base::Platform::MACOSX) {
    EXPECT_EQ((Pair<String>("/work/tree", ".")), KeyRoot("/work/tree", "a.cc"));
  } else {
    EXPECT_EQ((Pair<String>("", "")), KeyRoot("/work/tree", "a.cc"));
  }
}

TEST(EmitterSourceRootsTest, KeyRootIsRemapped) {
  google::protobuf::RepeatedPtrField<String> roots;
  *roots.Add() = "/work/tree";

  auto Flags = [&](const String& current_dir, const String& input,
                   const List<String>& remapped) {
    base::proto::Flags flags;
    flags.set_input(input);
    for (const auto& prefix : remapped) {
      flags.add_remapped_prefixes(prefix);
    }
    Emitter::SetKeyRoot(roots, current_dir, &flags);
    return flags;
  };

  // The maps don't remove the root from the paths, that the compiler sees.
  auto flags = Flags("/work/tree/out", "/work/tree/a.cc", {"/nonexistent"});
  EXPECT_FALSE(flags.has_key_root());

  // The build directory at another depth: the relative paths keep a part of
  // the root.
  flags = Flags("/work/tree/out/x/y", "../../../a.cc", {"../.."});
  EXPECT_FALSE(flags.has_key_root());

  // Only the remapped forms of the root are replaced in the key.
  flags = Flags("/work/tree/out/x", "../../a.cc", {"../.."});
  EXPECT_EQ("/work/tree", flags.key_root());
  EXPECT_FALSE(flags.key_root_remapped());
  EXPECT_TRUE(flags.key_root_relative_remapped());

  flags = Flags("/work/tree/out/x", "/work/tree/a.cc", {"/work/tree", "../.."});
  EXPECT_TRUE(flags.key_root_remapped());
  EXPECT_TRUE(flags.key_root_relative_remapped());
}

TEST(EmitterSourceRootsTest, PchKey) {
  google::protobuf::RepeatedPtrField<String> roots;
  *roots.Add() = "/work/tree";

  auto Flags = [&](const String& current_dir, const String& input,
                   const String& sysroot, bool relocatable = true,
                   const List<String>& remapped = {"/work/tree", ".."}) {
    base::proto::Flags flags;
    flags.set_input(input);
    flags.set_pch(true);
    flags.set_relocatable(relocatable);
    if (!sysroot.empty()) {
      flags.set_pch_sysroot(sysroot);
    }
    for (const auto& prefix : remapped) {
      flags.add_remapped_prefixes(prefix);
    }
    Emitter::SetPchKey(roots, current_dir, &flags);
    return flags;
  };
  auto KeyExtra = [](const base::proto::Flags& flags) {
    return List<String>(flags.key_extra().begin(), flags.key_extra().end());
  };

  // The paths in the header are relative to the root - also the ones of the
  // build directory: its place in the root is a part of the key.
  auto flags = Flags("/work/tree/out", "../a.h", "/work/tree/");
  EXPECT_EQ("/work/tree", flags.key_root());
  EXPECT_EQ((List<String>{"pch-cwd:out"}), KeyExtra(flags));

  flags = Flags("/work/tree", "a.h", "/work/tree", true, {"/work/tree"});
  EXPECT_EQ("/work/tree", flags.key_root());
  EXPECT_EQ((List<String>{"pch-cwd:."}), KeyExtra(flags));

  // Otherwise the paths are absolute: the root isn't replaced in the key, and
  // the current directory is a part of it.
  const List<base::proto::Flags> absolute = {
      Flags("/work/tree/out", "../a.h", ""),
      Flags("/work/tree/out", "../a.h", "/work"),
      Flags("/work/tree/out", "../a.h", "/"),
      Flags("/work/tree/out", "../a.h", "/work/tree/sub"),
      Flags("/work/tree/out", "../a.h", ".."),
      Flags("/work/tree/out", "../a.h", "/work/tree/out/.."),
      Flags("/work/tree/out", "../a.h", "/work/tree", false),
      Flags("/work/tree/out", "../a.h", "/work/tree", true, {}),
      Flags("/work/tree/out", "/other/a.h", "/work/tree"),
  };
  for (const auto& flags : absolute) {
    EXPECT_FALSE(flags.has_key_root()) << flags.DebugString();
    EXPECT_EQ((List<String>{"pch-cwd:/work/tree/out"}), KeyExtra(flags))
        << flags.DebugString();
  }

  // The build directory outside the root.
  flags = Flags("/work/out", "../tree/a.h", "/work/tree", true,
                {"/work/tree", "../tree"});
  EXPECT_FALSE(flags.has_key_root());
  EXPECT_EQ((List<String>{"pch-cwd:/work/out"}), KeyExtra(flags));
}

}  // namespace daemon
}  // namespace dist_clang
