#include "base/constants.h"
#include "base/file_utils.h"
#include "base/temporary_dir.h"
#include "client/clang.h"
#include "client/clang_flag_set.h"
#include "gtest/gtest.h"
#include "net/network_service_impl.h"
#include "net/test_network_service.h"
#include "proto/remote.pb.h"

namespace dist_clang {
namespace client {

TEST(ClangFlagSetTest, SimpleInput) {
  std::list<std::string> input = {
    "",
    "/home/test/.local/bin/clang", "-cc1",
    "-triple", "x86_64-unknown-linux-gnu",
    "-emit-obj",
    "-mrelax-all",
    "-disable-free",
    "-main-file-name", "test.cc",
    "-mrelocation-model", "static",
    "-mdisable-fp-elim",
    "-fmath-errno",
    "-masm-verbose",
    "-mconstructor-aliases",
    "-munwind-tables",
    "-fuse-init-array",
    "-target-cpu", "x86-64",
    "-target-linker-version", "2.23.2",
    "-coverage-file", "/tmp/test.o",
    "-resource-dir", "/home/test/.local/lib/clang/3.4",
    "-internal-isystem", "/usr/include/c++/4.8.2",
    "-internal-isystem", "/usr/include/c++/4.8.2/x86_64-redhat-linux",
    "-internal-isystem", "/usr/include/c++/4.8.2/backward",
    "-internal-isystem", "/usr/include/x86_64-redhat-linux/c++/4.8.2",
    "-internal-isystem", "/usr/local/include",
    "-internal-isystem", "/home/test/.local/lib/clang/3.4/include",
    "-internal-externc-isystem", "/include",
    "-internal-externc-isystem", "/usr/include",
    "-fdeprecated-macro",
    "-fdebug-compilation-dir", "/tmp",
    "-ferror-limit", "19",
    "-fmessage-length", "213",
    "-mstackrealign",
    "-fobjc-runtime=gcc",
    "-fcxx-exceptions",
    "-fexceptions",
    "-fdiagnostics-show-option",
    "-fcolor-diagnostics",
    "-vectorize-slp",
    "-o", "test.o",
    "-x", "c++",
    "test.cc"
  };

  proto::Flags expected_flags;
  expected_flags.mutable_compiler()->set_path("/home/test/.local/bin/clang");
  expected_flags.mutable_compiler()->set_version("0.0");
  expected_flags.set_output("test.o");
  expected_flags.set_input("test.cc");
  expected_flags.set_language("c++");
  expected_flags.add_other()->assign("-cc1");
  expected_flags.add_other()->assign("-triple");
  expected_flags.add_other()->assign("x86_64-unknown-linux-gnu");
  expected_flags.add_other()->assign("-emit-obj");
  expected_flags.add_other()->assign("-mrelax-all");
  expected_flags.add_other()->assign("-disable-free");
  expected_flags.add_other()->assign("-mrelocation-model");
  expected_flags.add_other()->assign("static");
  expected_flags.add_other()->assign("-mdisable-fp-elim");
  expected_flags.add_other()->assign("-fmath-errno");
  expected_flags.add_other()->assign("-masm-verbose");
  expected_flags.add_other()->assign("-mconstructor-aliases");
  expected_flags.add_other()->assign("-munwind-tables");
  expected_flags.add_other()->assign("-fuse-init-array");
  expected_flags.add_other()->assign("-target-cpu");
  expected_flags.add_other()->assign("x86-64");
  expected_flags.add_other()->assign("-target-linker-version");
  expected_flags.add_other()->assign("2.23.2");
  expected_flags.add_other()->assign("-fdeprecated-macro");
  expected_flags.add_other()->assign("-fmessage-length");
  expected_flags.add_other()->assign("213");
  expected_flags.add_other()->assign("-mstackrealign");
  expected_flags.add_other()->assign("-fobjc-runtime=gcc");
  expected_flags.add_other()->assign("-fcxx-exceptions");
  expected_flags.add_other()->assign("-fexceptions");
  expected_flags.add_other()->assign("-fdiagnostics-show-option");
  expected_flags.add_other()->assign("-fcolor-diagnostics");
  expected_flags.add_other()->assign("-vectorize-slp");
  expected_flags.add_non_cached()->assign("-main-file-name");
  expected_flags.add_non_cached()->assign("test.cc");
  expected_flags.add_non_cached()->assign("-coverage-file");
  expected_flags.add_non_cached()->assign("/tmp/test.o");
  expected_flags.add_non_cached()->assign("-resource-dir");
  expected_flags.add_non_cached()->assign("/home/test/.local/lib/clang/3.4");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/include/c++/4.8.2");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/usr/include/c++/4.8.2/x86_64-redhat-linux");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/include/c++/4.8.2/backward");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/usr/include/x86_64-redhat-linux/c++/4.8.2");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign("/usr/local/include");
  expected_flags.add_non_cached()->assign("-internal-isystem");
  expected_flags.add_non_cached()->assign(
      "/home/test/.local/lib/clang/3.4/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/include");
  expected_flags.add_non_cached()->assign("-internal-externc-isystem");
  expected_flags.add_non_cached()->assign("/usr/include");
  expected_flags.add_non_cached()->assign("-fdebug-compilation-dir");
  expected_flags.add_non_cached()->assign("/tmp");
  expected_flags.add_non_cached()->assign("-ferror-limit");
  expected_flags.add_non_cached()->assign("19");

  proto::Flags actual_flags;
  actual_flags.mutable_compiler()->set_version("0.0");
  ASSERT_EQ(ClangFlagSet::COMPILE,
            ClangFlagSet::ProcessFlags(input, &actual_flags));
  ASSERT_EQ(expected_flags.SerializeAsString(),
            actual_flags.SerializeAsString());
}

class ClientTest: public ::testing::Test {
  public:
    virtual void SetUp() override {
      using Service = net::TestNetworkService<>;

      send_count = 0, read_count = 0, connect_count = 0;
      connections_created = 0;
      custom_callback = [](net::TestConnection*) {};

      auto factory = net::NetworkService::SetFactory<Service::Factory>();
      factory->CallOnCreate([this](Service* service) {
        service->CountConnectAttempts(&connect_count);
        service->CallOnConnect([this](net::TestConnection* connection) {
          connection->CountSendAttempts(&send_count);
          connection->CountReadAttempts(&read_count);
          weak_ptr = connection->shared_from_this();
          ++connections_created;
          custom_callback(connection);
        });
      });
    }

  protected:
    net::ConnectionWeakPtr weak_ptr;
    uint send_count, read_count, connect_count, connections_created;
    std::function<void(net::TestConnection*)> custom_callback;
};

TEST_F(ClientTest, NoConnection) {
  using Service = net::TestNetworkService<false>;

  std::weak_ptr<net::Connection> weak_ptr;
  auto send_count = 0u, read_count = 0u, connect_count = 0u,
      connections_created = 0u;
  auto factory = net::NetworkService::SetFactory<Service::Factory>();
  factory->CallOnCreate([&](Service* service) {
    service->CountConnectAttempts(&connect_count);
    service->CallOnConnect([&](net::TestConnection* connection) {
      connection->CountSendAttempts(&send_count);
      connection->CountReadAttempts(&read_count);
      weak_ptr = connection->shared_from_this();
      ++connections_created;
    });
  });

  int argc = 3;
  const char* argv[] = {"a", "b", "c", nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", "clang_path"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, NoEnvironmentVariable) {
  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), std::string()));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(0u, connect_count);
  EXPECT_EQ(0u, connections_created);
}

TEST_F(ClientTest, NoInputFile) {
  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_FALSE(base::FileExists(test_file))
      << "Looks like someone is trying to sabotage this test!";
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, "socket_path", "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(0u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, CannotSendMessage) {
  custom_callback = [](net::TestConnection* connection) {
    connection->AbortOnSend();
  };

  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(0u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, CannotReadMessage) {
  base::TemporaryDir tmp_dir;
  std::string test_file_base = "test";
  auto test_file_name = test_file_base + ".cc";
  auto test_file = tmp_dir.GetPath() + "/" + test_file_name;

  custom_callback = [&](net::TestConnection* connection) {
    connection->AbortOnRead();
    connection->CallOnSend([&](const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(proto::Execute::extension));

      const auto& extension = message.GetExtension(proto::Execute::extension);
      EXPECT_FALSE(extension.remote());
      EXPECT_EQ(base::GetCurrentDir(), extension.current_dir());
      ASSERT_TRUE(extension.has_cc_flags());
      ASSERT_TRUE(extension.has_pp_flags());

      const auto& cc_flags = extension.cc_flags();
      ASSERT_TRUE(cc_flags.has_compiler());
      // TODO: check compiler version and path.
      EXPECT_EQ(test_file_base + ".o", cc_flags.output());
      EXPECT_EQ(test_file, cc_flags.input());
      EXPECT_EQ("c++", cc_flags.language());

      {
        const auto& other = cc_flags.other();
        auto begin = other.begin();
        auto end = other.end();
        EXPECT_NE(end, std::find(begin, end, "-cc1"));
        EXPECT_NE(end, std::find(begin, end, "-triple"));
        EXPECT_NE(end, std::find(begin, end, "-emit-obj"));
        EXPECT_NE(end, std::find(begin, end, "-target-cpu"));
        EXPECT_NE(end, std::find(begin, end, "-target-linker-version"));
      }

      {
        const auto& non_cached = cc_flags.non_cached();
        auto begin = non_cached.begin();
        auto end = non_cached.end();
        EXPECT_NE(end, std::find(begin, end, "-main-file-name"));
        EXPECT_NE(end, std::find(begin, end, test_file_name));
        EXPECT_NE(end, std::find(begin, end, "-coverage-file"));
        EXPECT_NE(end, std::find(begin, end, "-resource-dir"));
        EXPECT_NE(end, std::find(begin, end, "-internal-isystem"));
        EXPECT_NE(end, std::find(begin, end, "-internal-externc-isystem"));
      }

      const auto& pp_flags = extension.pp_flags();
      ASSERT_TRUE(pp_flags.has_compiler());
      // TODO: check compiler version and path.
      EXPECT_FALSE(pp_flags.has_output());
      EXPECT_EQ(test_file, pp_flags.input());
      EXPECT_EQ("c++", pp_flags.language());

      {
        const auto& other = pp_flags.other();
        auto begin = other.begin();
        auto end = other.end();
        EXPECT_NE(end, std::find(begin, end, "-cc1"));
        EXPECT_NE(end, std::find(begin, end, "-triple"));
        EXPECT_NE(end, std::find(begin, end, "-E"));
        EXPECT_NE(end, std::find(begin, end, "-target-cpu"));
        EXPECT_NE(end, std::find(begin, end, "-target-linker-version"));
      }

      {
        const auto& non_cached = pp_flags.non_cached();
        auto begin = non_cached.begin();
        auto end = non_cached.end();
        EXPECT_NE(end, std::find(begin, end, "-main-file-name"));
        EXPECT_NE(end, std::find(begin, end, test_file_name));
        EXPECT_NE(end, std::find(begin, end, "-coverage-file"));
        EXPECT_NE(end, std::find(begin, end, "-resource-dir"));
        EXPECT_NE(end, std::find(begin, end, "-internal-isystem"));
        EXPECT_NE(end, std::find(begin, end, "-internal-externc-isystem"));
      }
    });
  };

  int argc = 3;
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithoutStatus) {
  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, ReadMessageWithBadStatus) {
  custom_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::INCONSEQUENT);
    });
  };

  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_TRUE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, SuccessfulCompilation) {
  custom_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::OK);
    });
  };

  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_FALSE(client::DoMain(argc, argv, std::string(), "clang++"));
  EXPECT_TRUE(weak_ptr.expired());
  EXPECT_EQ(1u, send_count);
  EXPECT_EQ(1u, read_count);
  EXPECT_EQ(1u, connect_count);
  EXPECT_EQ(1u, connections_created);
}

TEST_F(ClientTest, FailedCompilation) {
  custom_callback = [](net::TestConnection* connection) {
    connection->CallOnRead([](net::Connection::Message* message) {
      auto extension = message->MutableExtension(proto::Status::extension);
      extension->set_code(proto::Status::EXECUTION);
    });
  };

  int argc = 3;
  base::TemporaryDir tmp_dir;
  auto test_file = tmp_dir.GetPath() + "/test.cc";
  ASSERT_TRUE(base::WriteFile(test_file, "int main() { return 0; }"));
  const char* argv[] = {"clang++", "-c", test_file.c_str(), nullptr};

  EXPECT_EXIT(client::DoMain(argc, argv, std::string(), "clang++"),
              ::testing::ExitedWithCode(1), ".*");
}

}  // namespace client
}  // namespace dist_clang
