#include "base/assert.h"
#include "base/c_utils.h"
#include "base/string_utils.h"
#include "net/base/utils.h"
#include "net/connection.h"
#include "net/event_loop.h"

#include <gtest/gtest.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace dist_clang {
namespace testing {

class TestMessage {
  public:
    TestMessage()
      : expected_field1_("arg" + base::IntToString(number_++)),
        expected_field2_("arg" + base::IntToString(number_++)),
        expected_field3_("arg" + base::IntToString(number_++)) {
      auto message = message_.MutableExtension(proto::Test::test);
      message->set_field1(expected_field1_);
      message->set_field2(expected_field2_);
      message->add_field3()->assign(expected_field3_);
    }

    net::Connection::Message& GetTestMessage() {
      return message_;
    }

    void CheckTestMessage(const net::Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(proto::Test::test));
      auto test = message.GetExtension(proto::Test::test);
      ASSERT_TRUE(test.has_field1());
      ASSERT_TRUE(test.has_field2());
      ASSERT_EQ(1, test.field3_size());
      EXPECT_EQ(expected_field1_, test.field1());
      EXPECT_EQ(expected_field2_, test.field2());
      EXPECT_EQ(expected_field3_, test.field3(0));
    }

  private:
    static int number_;
    const std::string expected_field1_, expected_field2_, expected_field3_;
    net::Connection::Message message_;
};

int TestMessage::number_ = 1;

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
          socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
      if (-1 == listen_fd_)
        return false;
      if (-1 == bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)))
        return false;
      if (-1 == listen(listen_fd_, 5))
        return false;

      return true;
    }
    TestServer()
      : listen_fd_(-1), server_fd_(-1),
        epoll_fd_(epoll_create1(EPOLL_CLOEXEC)) {
    }
    ~TestServer() {
      if (!socket_path_.empty())
        unlink(socket_path_.c_str());
      if (!tmp_path_.empty())
        rmdir(tmp_path_.c_str());
      if (listen_fd_ != -1)
        close(listen_fd_);
      if (server_fd_ != -1)
        close(server_fd_);
      Stop();
    }

    net::ConnectionPtr GetConnection() {
      sockaddr_un address;
      address.sun_family = AF_UNIX;
      strcpy(address.sun_path, socket_path_.c_str());

      int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
      if (-1 == fd) {
        std::cerr << strerror(errno) << std::endl;
        return net::ConnectionPtr();
      }

      auto connection = net::Connection::Create(*this, fd);

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

    void CloseServerConnection() {
      close(server_fd_);
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
      if (send(server_fd_, data.data(), data.size(), 0) !=
          static_cast<int>(data.size())) {
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

    bool WriteIncomplete(const std::string& data, size_t size) {
      base::Assert(size < data.size());
      if (data.size() > 127) {
        std::cerr << "Use messages with size less then 127" << std::endl;
        return false;
      }

      unsigned char data_size = data.size();
      if (send(server_fd_, &data_size, 1, 0) != 1) {
        std::cerr << strerror(errno) << std::endl;
        return false;
      }
      if (send(server_fd_, data.data(), size, 0) != static_cast<int>(size)) {
        std::cerr << strerror(errno) << std::endl;
        return false;
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
      struct epoll_event event;
      event.events = EPOLLIN | EPOLLONESHOT;
      event.data.ptr = connection.get();
      auto fd = GetConnectionDescriptor(connection);
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        base::Assert(errno == ENOENT);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
          return false;
        }
      }
      return true;
    }

    virtual bool ReadyForSend(net::ConnectionPtr connection) override {
      struct epoll_event event;
      event.events = EPOLLOUT | EPOLLONESHOT;
      event.data.ptr = connection.get();
      auto fd = GetConnectionDescriptor(connection);
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        base::Assert(errno == ENOENT);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
          return false;
        }
      }
      return true;
    }

    virtual void RemoveConnection(net::fd_t fd) {
      // Do nothing.
    }

    virtual void DoListenWork(const volatile bool& is_shutting_down) override {
      // Test server doesn't do listening work.
    }

    virtual void DoIOWork(const volatile bool& is_shutting_down) override {
      const int MAX_EVENTS = 10;  // This should be enought in most cases.
      const int TIMEOUT = 1 * 1000;  // In milliseconds.
      struct epoll_event events[MAX_EVENTS];

      while(!is_shutting_down) {
        auto events_count = epoll_wait(epoll_fd_, events, MAX_EVENTS, TIMEOUT);
        if (events_count == -1 && errno != EINTR) {
          break;
        }

        for (int i = 0; i < events_count; ++i) {
          auto ptr = reinterpret_cast<net::Connection*>(events[i].data.ptr);
          auto connection = ptr->shared_from_this();

          if (events[i].events & (EPOLLHUP|EPOLLERR)) {
            auto fd = GetConnectionDescriptor(connection);
            base::Assert(!epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
          }

          if (events[i].events & EPOLLIN) {
            ConnectionDoRead(connection);
          }
          else if (events[i].events & EPOLLOUT) {
            ConnectionDoSend(connection);
          }
        }
      }
    }

    int listen_fd_, server_fd_, epoll_fd_;
    std::string tmp_path_, socket_path_;
};

class ConnectionTest: public ::testing::Test {
  public:
    void SetUp() override {
      ASSERT_TRUE(server.Init());
      connection = server.GetConnection();
      ASSERT_TRUE(!!connection);
    }
    void TearDown() override {
      connection.reset();
    }

  protected:
    net::ConnectionPtr connection;
    TestServer server;
};

TEST_F(ConnectionTest, Sync_ReadOneMessage) {
  TestMessage test_message;
  auto& expected_message = test_message.GetTestMessage();
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializeAsString()));

  net::Connection::Message message;
  proto::Status status;

  ASSERT_TRUE(connection->ReadSync(&message, &status)) << status.description();

  EXPECT_EQ(proto::Status::OK, status.code());
  EXPECT_FALSE(status.has_description());
  test_message.CheckTestMessage(message);
}

TEST_F(ConnectionTest, Sync_ReadTwoMessages) {
  TestMessage test_message1, test_message2;
  auto& expected_message1 = test_message1.GetTestMessage();
  auto& expected_message2 = test_message2.GetTestMessage();
  ASSERT_TRUE(server.WriteAtOnce(expected_message1.SerializeAsString()));
  ASSERT_TRUE(server.WriteAtOnce(expected_message2.SerializeAsString()));

  net::Connection::Message message;
  proto::Status status;

  ASSERT_TRUE(connection->ReadSync(&message, &status)) << status.description();

  EXPECT_EQ(proto::Status::OK, status.code());
  EXPECT_FALSE(status.has_description());
  test_message1.CheckTestMessage(message);

  ASSERT_TRUE(connection->ReadSync(&message, &status)) << status.description();

  EXPECT_EQ(proto::Status::OK, status.code());
  EXPECT_FALSE(status.has_description());
  test_message2.CheckTestMessage(message);
}

TEST_F(ConnectionTest, Sync_ReadSplitMessage) {
  TestMessage test_message;

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Status status;

    ASSERT_TRUE(connection->ReadSync(&message, &status))
        << status.description();

    EXPECT_EQ(proto::Status::OK, status.code());
    EXPECT_FALSE(status.has_description());
    test_message.CheckTestMessage(message);
  };

  auto& expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(server.WriteByParts(expected_message.SerializeAsString()));
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadUninitializedMessage) {
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
  proto::Status status;

  ASSERT_FALSE(connection->ReadSync(&message, &status));
  EXPECT_EQ(proto::Status::BAD_MESSAGE, status.code());
}

TEST_F(ConnectionTest, Sync_SendMessage) {
  TestMessage test_message;
  auto& expected_message = test_message.GetTestMessage();
  proto::Status status;
  ASSERT_TRUE(connection->SendSync(expected_message, &status))
      << status.description();
  std::string data;
  ASSERT_TRUE(server.ReadAtOnce(data));

  net::Connection::Message message;
  message.ParseFromString(data);
  test_message.CheckTestMessage(message);
}

TEST_F(ConnectionTest, Sync_ReadIncompleteMessage) {
  TestMessage test_message;

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Status status;

    ASSERT_FALSE(connection->ReadSync(&message, &status));

    EXPECT_EQ(proto::Status::BAD_MESSAGE, status.code());
    EXPECT_TRUE(status.has_description());
  };

  auto& expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(server.WriteIncomplete(expected_message.SerializeAsString(),
                                     expected_message.ByteSize() / 2));
  // TODO: close connection.
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadFromClosedConnection) {
  net::Connection::Message message;
  proto::Status status;

  // TODO: close connection.
  ASSERT_FALSE(connection->ReadSync(&message, &status));
  EXPECT_EQ(proto::Status::NETWORK, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_ReadAfterClosingConnectionOnServerSide) {
  TestMessage test_message;

  auto read_func = [&] () {
    net::Connection::Message message;
    proto::Status status;

    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(connection->ReadSync(&message, &status))
        << status.description();

    EXPECT_EQ(proto::Status::OK, status.code());
    EXPECT_FALSE(status.has_description());
    test_message.CheckTestMessage(message);
  };

  auto& expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  ASSERT_TRUE(server.WriteAtOnce(expected_message.SerializeAsString()));
  server.CloseServerConnection();
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_SendToClosedConnection) {
  TestMessage message;
  auto& expected_message = message.GetTestMessage();
  proto::Status status;

  // TODO: close connection.
  ASSERT_FALSE(connection->SendSync(expected_message, &status));
  EXPECT_EQ(proto::Status::NETWORK, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_SendAfterClosingConnectionOnServerSide) {
  TestMessage message;
  auto& expected_message = message.GetTestMessage();
  proto::Status status;

  server.CloseServerConnection();
  ASSERT_FALSE(connection->SendSync(expected_message, &status));
  EXPECT_EQ(proto::Status::NETWORK, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_SendSubMessages) {
  TestMessage test_message;
  auto& expected_message =
      test_message.GetTestMessage().GetExtension(proto::Test::test);
  proto::Status status;
  ASSERT_TRUE(connection->SendSync(expected_message, &status))
      << status.description();
  std::string data;
  ASSERT_TRUE(server.ReadAtOnce(data));

  net::Connection::Message message;
  message.ParseFromString(data);
  test_message.CheckTestMessage(message);

  proto::TestNotExtension bad_message;
  bad_message.set_field1("arg1");
  bad_message.set_field2("arg2");
  bad_message.add_field3("arg3");
  ASSERT_FALSE(connection->SendSync(bad_message, &status));
}

}  // namespace testing
}  // namespace dist_clang
