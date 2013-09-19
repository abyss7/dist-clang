#include "base/c_utils.h"
#include "net/base/utils.h"
#include "net/connection.h"
#include "net/event_loop.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace dist_clang {
namespace testing {

class TestServer: public net::EventLoop {
  public:
    bool Init() {
      tmp_path_ = base::CreateTempDir();
      if (tmp_path_.empty())
        return false;
      socket_path_ = tmp_path_ + "/socket";

      sockaddr_un address;
      address.sun_family = AF_UNIX;
      strcpy(address.sun_path, socket_path_.c_str());

      listen_fd_ =
          socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (-1 == listen_fd_)
        return false;
      if (-1 == bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)))
        return false;
      if (-1 == listen(listen_fd_, 5))
        return false;

      return Run();
    }
    TestServer()
      : listen_fd_(-1), server_fd_(-1) {}
    ~TestServer() {
      if (!socket_path_.empty())
        unlink(socket_path_.c_str());
      if (!tmp_path_.empty())
        rmdir(tmp_path_.c_str());
      if (listen_fd_ != -1)
        close(listen_fd_);
      if (server_fd_ != -1)
        close(server_fd_);
    }

    net::ConnectionPtr GetConnection() {
      sockaddr_un address;
      address.sun_family = AF_UNIX;
      strcpy(address.sun_path, socket_path_.c_str());

      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (-1 == fd) {
        std::cerr << strerror(errno) << std::endl;
        return net::ConnectionPtr();
      }

      net::ConnectionPtr connection = net::Connection::Create(*this, fd);
      if (!connection) {
        std::cerr << "Failed to create connection" << std::endl;
        close(fd);
        return net::ConnectionPtr();
      }

      if (connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == -1 && errno != EINPROGRESS) {
        std::cerr << strerror(errno) << std::endl;
        return net::ConnectionPtr();
      }

      server_fd_ = accept(listen_fd_, nullptr, nullptr);
      if (server_fd_ == -1) {
        std::cerr << strerror(errno) << std::endl;
        return net::ConnectionPtr();
      }

      return connection;
    }

    bool WriteAtOnce(const std::string& data) {
      if (data.size() > 127) {
        std::cerr << "Use messages with size less then 127" << std::endl;
        return false;
      }
      unsigned char size = data.size();
      if (send(server_fd_, &size, 1, 0) != 1) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }
      if (send(server_fd_, data.data(), data.size(), 0) != data.size()) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }

      return true;
    }

    bool WriteByParts(const std::string& data) {
      if (data.size() > 127) {
        std::cerr << "Use messages with size less then 127" << std::endl;
        return false;
      }
      unsigned char size = data.size();
      if (send(server_fd_, &size, 1, 0) != 1) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }
      for (size_t i = 0; i < size; ++i) {
        if (send(server_fd_, data.data() + i, 1, 0) != 1) {
          std::cerr << strerror(errno) << std::endl;
          return false;
        }
      }

      return true;
    }

    bool ReadAtOnce(std::string& data) {
      unsigned char size = 0;
      if (recv(server_fd_, &size, 1, 0) != 1) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }
      char buf[128];
      if (recv(server_fd_, buf, size, 0) != size) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }
      data.assign(std::string(buf, size));

      return true;
    }

  private:
    virtual bool ReadyForRead(net::ConnectionPtr connection) override {
      // TODO: implement this.
      return false;
    }
    virtual bool ReadyForSend(net::ConnectionPtr connection) override {
      // TODO: implement this.
      return false;
    }
    virtual void DoListenWork(const volatile bool &is_shutting_down) override {
      // TODO: implement this.
    }
    virtual void
    DoIncomingWork(const volatile bool &is_shutting_down) override {
      // TODO: implement this.
    }
    virtual void
    DoOutgoingWork(const volatile bool &is_shutting_down) override {
      // TODO: implement this.
    }

    int listen_fd_, server_fd_;
    std::string tmp_path_, socket_path_;
};

class ConnectionTest: public ::testing::Test {
  public:
    void SetUp() override {
      ASSERT_TRUE(server.Init());
      connection = server.GetConnection();
      ASSERT_TRUE(static_cast<bool>(connection));
    }
    void TearDown() override {
      connection.reset();
    }

  protected:
    net::ConnectionPtr connection;
    TestServer server;
};

TEST_F(ConnectionTest, Sync_ReadOneMessage) {
  const std::string expected_field1 = "arg1";
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializeAsString()));

  net::Connection::Message message;
  proto::Error error;

  ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

  // Check error.
  EXPECT_EQ(proto::Error::OK, error.code());
  EXPECT_FALSE(error.has_description());

  // Check incoming message.
  ASSERT_TRUE(message.HasExtension(proto::Test::test));
  auto test = message.GetExtension(proto::Test::test);
  ASSERT_TRUE(test.has_field1());
  ASSERT_TRUE(test.has_field2());
  ASSERT_EQ(1, test.field3_size());
  EXPECT_EQ(expected_field1, test.field1());
  EXPECT_EQ(expected_field2, test.field2());
  EXPECT_EQ(expected_field3, test.field3(0));
}

TEST_F(ConnectionTest, Sync_ReadTwoMessages) {
  const std::string expected_field1_1 = "arg1";
  const std::string expected_field2_1 = "arg2";
  const std::string expected_field3_1 = "arg3";
  const std::string expected_field1_2 = "arg4";
  const std::string expected_field2_2 = "arg5";
  const std::string expected_field3_2 = "arg6";

  net::Connection::Message expected_message1, expected_message2;
  {
    auto message = expected_message1.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1_1);
    message->set_field2(expected_field2_1);
    message->add_field3()->assign(expected_field3_1);

    message = expected_message2.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1_2);
    message->set_field2(expected_field2_2);
    message->add_field3()->assign(expected_field3_2);
  }
  ASSERT_TRUE(server.WriteAtOnce(expected_message1.SerializeAsString()));
  ASSERT_TRUE(server.WriteAtOnce(expected_message2.SerializeAsString()));

  net::Connection::Message message;
  proto::Error error;

  ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

  // Check error.
  EXPECT_EQ(proto::Error::OK, error.code());
  EXPECT_FALSE(error.has_description());

  // Check incoming message.
  ASSERT_TRUE(message.HasExtension(proto::Test::test));
  auto test = message.GetExtension(proto::Test::test);
  ASSERT_TRUE(test.has_field1());
  ASSERT_TRUE(test.has_field2());
  ASSERT_EQ(1, test.field3_size());
  EXPECT_EQ(expected_field1_1, test.field1());
  EXPECT_EQ(expected_field2_1, test.field2());
  EXPECT_EQ(expected_field3_1, test.field3(0));

  ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

  // Check error.
  EXPECT_EQ(proto::Error::OK, error.code());
  EXPECT_FALSE(error.has_description());

  // Check incoming message.
  ASSERT_TRUE(message.HasExtension(proto::Test::test));
  test = message.GetExtension(proto::Test::test);
  ASSERT_TRUE(test.has_field1());
  ASSERT_TRUE(test.has_field2());
  ASSERT_EQ(1, test.field3_size());
  EXPECT_EQ(expected_field1_2, test.field1());
  EXPECT_EQ(expected_field2_2, test.field2());
  EXPECT_EQ(expected_field3_2, test.field3(0));
}

TEST_F(ConnectionTest, Sync_ReadSplitMessage) {
  const std::string expected_field1 = "arg1";
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Error error;

    ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

    // Check error.
    EXPECT_EQ(proto::Error::OK, error.code());
    EXPECT_FALSE(error.has_description());

    // Check incoming message.
    ASSERT_TRUE(message.HasExtension(proto::Test::test));
    auto test = message.GetExtension(proto::Test::test);
    ASSERT_TRUE(test.has_field1());
    ASSERT_TRUE(test.has_field2());
    ASSERT_EQ(1, test.field3_size());
    EXPECT_EQ(expected_field1, test.field1());
    EXPECT_EQ(expected_field2, test.field2());
    EXPECT_EQ(expected_field3, test.field3(0));
  };

  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(server.WriteByParts(expected_message.SerializeAsString()));
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadIncompleteMessage) {
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializePartialAsString()));

  net::Connection::Message message;
  proto::Error error;

  ASSERT_FALSE(connection->ReadSync(&message, &error));
  EXPECT_EQ(proto::Error::BAD_MESSAGE, error.code());
}

TEST_F(ConnectionTest, Sync_ReadWhileReading) {
  const std::string expected_field1 = "arg1";
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Error error;

    ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

    // Check error.
    EXPECT_EQ(proto::Error::OK, error.code());
    EXPECT_FALSE(error.has_description());

    // Check incoming message.
    ASSERT_TRUE(message.HasExtension(proto::Test::test));
    auto test = message.GetExtension(proto::Test::test);
    ASSERT_TRUE(test.has_field1());
    ASSERT_TRUE(test.has_field2());
    ASSERT_EQ(1, test.field3_size());
    EXPECT_EQ(expected_field1, test.field1());
    EXPECT_EQ(expected_field2, test.field2());
    EXPECT_EQ(expected_field3, test.field3(0));
  };

  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  net::Connection::Message message;
  proto::Error error;
  ASSERT_FALSE(connection->ReadSync(&message, &error));
  EXPECT_EQ(proto::Error::INCONSEQUENT, error.code());
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializeAsString()));
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_SendMessage) {
  const std::string expected_field1 = "arg1";
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }
  proto::Error error;
  ASSERT_TRUE(connection->SendSync(expected_message, &error))
      << error.description();
  std::string data;
  ASSERT_TRUE(server.ReadAtOnce(data));

  net::Connection::Message message;
  message.ParseFromString(data);
  ASSERT_TRUE(message.HasExtension(proto::Test::test));
  auto test = message.GetExtension(proto::Test::test);
  ASSERT_TRUE(test.has_field1());
  ASSERT_TRUE(test.has_field2());
  ASSERT_EQ(1, test.field3_size());
  EXPECT_EQ(expected_field1, test.field1());
  EXPECT_EQ(expected_field2, test.field2());
  EXPECT_EQ(expected_field3, test.field3(0));
}

TEST_F(ConnectionTest, Sync_SendWhileReading) {
  const std::string expected_field1 = "arg1";
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::test);
    message->set_field1(expected_field1);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Error error;

    ASSERT_TRUE(connection->ReadSync(&message, &error)) << error.description();

    // Check error.
    EXPECT_EQ(proto::Error::OK, error.code());
    EXPECT_FALSE(error.has_description());

    // Check incoming message.
    ASSERT_TRUE(message.HasExtension(proto::Test::test));
    auto test = message.GetExtension(proto::Test::test);
    ASSERT_TRUE(test.has_field1());
    ASSERT_TRUE(test.has_field2());
    ASSERT_EQ(1, test.field3_size());
    EXPECT_EQ(expected_field1, test.field1());
    EXPECT_EQ(expected_field2, test.field2());
    EXPECT_EQ(expected_field3, test.field3(0));
  };

  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  proto::Error error;
  ASSERT_FALSE(connection->SendSync(expected_message, &error));
  EXPECT_EQ(proto::Error::INCONSEQUENT, error.code());
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializeAsString()));
  read_thread.join();
}

TEST_F(ConnectionTest, DISABLED_Sync_ReadFromClosedConnection) {
  // TODO: implement this.
}

TEST_F(ConnectionTest, DISABLED_Sync_SendToClosedConnection) {
  // TODO: implement this.
}

TEST_F(ConnectionTest, DISABLED_Sync_SendSubMessages) {
  // TODO: implement this.
  // Here should be tested the ability of SendSync() to send different
  // sub-messages, like Execute and Error.
}

}  // namespace testing
}  // namespace dist_clang
