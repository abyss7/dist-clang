#include "base/assert.h"
#include "base/c_utils.h"
#include "base/logging.h"
#include "base/string_utils.h"
#include "net/base/utils.h"
#include "net/connection.h"
#include "net/event_loop.h"

#include <gtest/gtest.h>
#include <memory>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "base/using_log.h"

namespace dist_clang {
namespace net {

class TestMessage {
  public:
    TestMessage()
      : expected_field1_("arg" + std::to_string(number_++)),
        expected_field2_("arg" + std::to_string(number_++)),
        expected_field3_("arg" + std::to_string(number_++)) {
      message_.reset(new Connection::Message);
      auto message = message_->MutableExtension(proto::Test::extension);
      message->set_field1(expected_field1_);
      message->set_field2(expected_field2_);
      message->add_field3()->assign(expected_field3_);
    }

    std::unique_ptr<Connection::Message> GetTestMessage() {
      return std::move(message_);
    }

    void CheckTestMessage(const Connection::Message& message) {
      ASSERT_TRUE(message.HasExtension(proto::Test::extension));
      auto test = message.GetExtension(proto::Test::extension);
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
    std::unique_ptr<Connection::Message> message_;
};

int TestMessage::number_ = 1;

class TestServer: public EventLoop {
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

    ConnectionPtr GetConnection() {
      sockaddr_un address;
      address.sun_family = AF_UNIX;
      strcpy(address.sun_path, socket_path_.c_str());

      int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
      if (-1 == fd) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return ConnectionPtr();
      }

      auto connection = Connection::Create(*this, fd);

      if (connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == -1 && errno != EINPROGRESS) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return ConnectionPtr();
      }

      server_fd_ = accept(listen_fd_, nullptr, nullptr);
      if (server_fd_ == -1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return ConnectionPtr();
      }

      return connection;
    }

    void CloseServerConnection() {
      close(server_fd_);
    }

    bool WriteAtOnce(const std::string& data) {
      if (data.size() > 127) {
        LOG(ERROR) << "Use messages with size less then 127" << std::endl;
        return false;
      }
      unsigned char size = data.size();
      if (send(server_fd_, &size, 1, 0) != 1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
      if (send(server_fd_, data.data(), data.size(), 0) !=
          static_cast<int>(data.size())) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }

      return true;
    }

    bool WriteByParts(const std::string& data) {
      if (data.size() > 127) {
        LOG(ERROR) << "Use messages with size less then 127" << std::endl;
        return false;
      }
      unsigned char size = data.size();
      if (send(server_fd_, &size, 1, 0) != 1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
      for (size_t i = 0; i < size; ++i) {
        if (send(server_fd_, data.data() + i, 1, 0) != 1) {
          LOG(ERROR) << strerror(errno) << std::endl;
          return false;
        }
      }

      return true;
    }

    bool WriteIncomplete(const std::string& data, size_t size) {
      DCHECK(size < data.size());
      if (data.size() > 127) {
        LOG(ERROR) << "Use messages with size less then 127" << std::endl;
        return false;
      }

      unsigned char data_size = data.size();
      if (send(server_fd_, &data_size, 1, 0) != 1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
      if (send(server_fd_, data.data(), size, 0) != static_cast<int>(size)) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }

      return true;
    }

    bool ReadAtOnce(std::string& data) {
      unsigned char size = 0;
      if (recv(server_fd_, &size, 1, 0) != 1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
      char buf[128];
      if (recv(server_fd_, buf, size, 0) != size) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
      data.assign(std::string(buf, size));

      return true;
    }

  private:
    virtual bool HandlePassive(fd_t fd) override {
      // TODO: implement this.
      return false;
    }

    virtual bool ReadyForRead(ConnectionPtr connection) override {
      struct epoll_event event;
      event.events = EPOLLIN | EPOLLONESHOT;
      event.data.ptr = connection.get();
      auto fd = GetConnectionDescriptor(connection);
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        DCHECK(errno == ENOENT);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
          return false;
        }
      }
      return true;
    }

    virtual bool ReadyForSend(ConnectionPtr connection) override {
      struct epoll_event event;
      event.events = EPOLLOUT | EPOLLONESHOT;
      event.data.ptr = connection.get();
      auto fd = GetConnectionDescriptor(connection);
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        DCHECK(errno == ENOENT);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
          return false;
        }
      }
      return true;
    }

    virtual void RemoveConnection(fd_t fd) {
      // Do nothing.
    }

    virtual void DoListenWork(const std::atomic<bool>& is_shutting_down,
                              fd_t self_pipe) override {
      // Test server doesn't do listening work.
    }

    virtual void DoIOWork(const std::atomic<bool>& is_shutting_down,
                          fd_t self_pipe) override {
      const int TIMEOUT = 1 * 1000;  // In milliseconds.
      struct epoll_event event;
      event.events = EPOLLIN;
      event.data.fd = self_pipe;
      epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, self_pipe, &event);

      while(!is_shutting_down) {
        auto events_count = epoll_wait(epoll_fd_, &event, 1, TIMEOUT);
        if (events_count == -1) {
          if (errno != EINTR) {
            break;
          }
          else {
            continue;
          }
        }

        DCHECK(events_count == 1);
        fd_t fd = event.data.fd;

        // FIXME: it's a little bit hacky, but should work almost always.
        if (fd == self_pipe) {
          continue;
        }

        auto ptr = reinterpret_cast<Connection*>(event.data.ptr);
        auto connection = ptr->shared_from_this();

        if (event.events & (EPOLLHUP|EPOLLERR)) {
          auto fd = GetConnectionDescriptor(connection);
          DCHECK_O_EVAL(!epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
        }

        if (event.events & EPOLLIN) {
          ConnectionDoRead(connection);
        }
        else if (event.events & EPOLLOUT) {
          ConnectionDoSend(connection);
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
  auto expected_message = test_message.GetTestMessage();
  ASSERT_TRUE(server.WriteAtOnce(expected_message->SerializeAsString()));

  Connection::Message message;
  proto::Status status;

  ASSERT_TRUE(connection->ReadSync(&message, &status)) << status.description();

  EXPECT_EQ(proto::Status::OK, status.code());
  EXPECT_FALSE(status.has_description());
  test_message.CheckTestMessage(message);
}

TEST_F(ConnectionTest, Sync_ReadTwoMessages) {
  TestMessage test_message1, test_message2;
  auto expected_message1 = test_message1.GetTestMessage();
  auto expected_message2 = test_message2.GetTestMessage();
  ASSERT_TRUE(server.WriteAtOnce(expected_message1->SerializeAsString()));
  ASSERT_TRUE(server.WriteAtOnce(expected_message2->SerializeAsString()));

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

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(server.WriteByParts(expected_message->SerializeAsString()));
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadUninitializedMessage) {
  const std::string expected_field2 = "arg2";
  const std::string expected_field3 = "arg3";

  net::Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::extension);
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
  auto expected_message = test_message.GetTestMessage();
  proto::Status status;
  ASSERT_TRUE(connection->SendSync(std::move(expected_message), &status))
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

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(server.WriteIncomplete(expected_message->SerializeAsString(),
                                     expected_message->ByteSize() / 2));
  connection->Close();
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadFromClosedConnection) {
  net::Connection::Message message;
  proto::Status status;

  connection->Close();
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

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  ASSERT_TRUE(server.WriteAtOnce(expected_message->SerializeAsString()));
  server.CloseServerConnection();
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_SendToClosedConnection) {
  TestMessage message;
  auto expected_message = message.GetTestMessage();
  proto::Status status;

  connection->Close();
  ASSERT_FALSE(connection->SendSync(std::move(expected_message), &status));
  EXPECT_EQ(proto::Status::NETWORK, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_SendAfterClosingConnectionOnServerSide) {
  TestMessage message;
  auto expected_message = message.GetTestMessage();
  proto::Status status;

  server.CloseServerConnection();
  ASSERT_FALSE(connection->SendSync(std::move(expected_message), &status));
  EXPECT_EQ(proto::Status::NETWORK, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_SendSubMessages) {
  TestMessage test_message;
  std::unique_ptr<proto::Test> expected_message;
  auto test_extension =
      test_message.GetTestMessage()->GetExtension(proto::Test::extension);
  expected_message.reset(new proto::Test(test_extension));
  proto::Status status;
  ASSERT_TRUE(connection->SendSync(std::move(expected_message), &status))
      << status.description();
  std::string data;
  ASSERT_TRUE(server.ReadAtOnce(data));

  net::Connection::Message message;
  message.ParseFromString(data);
  test_message.CheckTestMessage(message);
}

}  // namespace testing
}  // namespace dist_clang
