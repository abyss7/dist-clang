#include <base/assert.h>
#include <base/file/epoll.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>
#include <net/connection_impl.h>
#include <net/event_loop.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <base/using_log.h>

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

  UniquePtr<Connection::Message> GetTestMessage() {
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
  const String expected_field1_, expected_field2_, expected_field3_;
  Connection::ScopedMessage message_;
};

// static
int TestMessage::number_ = 1;

class TestServer : public EventLoop {
 public:
  bool Init() {
    if (tmp_dir_.GetPath().empty()) {
      return false;
    }
    socket_path_ = tmp_dir_.GetPath() + "/socket";

    sockaddr_un address;
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (-1 == listen_fd_) {
      return false;
    }
    if (-1 == bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address))) {
      return false;
    }
    if (-1 == listen(listen_fd_, 5)) {
      return false;
    }

    return true;
  }
  TestServer() : listen_fd_(-1), server_fd_(-1) {}
  ~TestServer() {
    if (listen_fd_ != -1) {
      close(listen_fd_);
    }
    if (server_fd_ != -1) {
      close(server_fd_);
    }
    Stop();
  }

  ConnectionImplPtr GetConnection() {
    sockaddr_un address;
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path_.c_str());

    Socket fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.IsValid()) {
      LOG(ERROR) << strerror(errno) << std::endl;
      return ConnectionImplPtr();
    }

    if (connect(fd.native(), reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == -1 &&
        errno != EINPROGRESS) {
      LOG(ERROR) << strerror(errno) << std::endl;
      return ConnectionImplPtr();
    }

    auto connection = ConnectionImpl::Create(*this, std::move(fd));

    server_fd_ = accept(listen_fd_, nullptr, nullptr);
    if (server_fd_ == -1) {
      LOG(ERROR) << strerror(errno) << std::endl;
      return ConnectionImplPtr();
    }

    return connection;
  }

  void CloseServerConnection() { close(server_fd_); }

  bool WriteAtOnce(const Connection::Message& message) {
    String gzipped_string;
    {
      using namespace google::protobuf::io;

      StringOutputStream string_stream(&gzipped_string);
      GzipOutputStream::Options options;
      options.format = GzipOutputStream::ZLIB;
      GzipOutputStream gzip_stream(&string_stream, options);
      {
        CodedOutputStream coded_stream(&gzip_stream);
        coded_stream.WriteVarint32(message.ByteSize());
      }
      message.SerializePartialToZeroCopyStream(&gzip_stream);
    }

    CHECK(gzipped_string.size() < 128);
    if (send(server_fd_, gzipped_string.data(), gzipped_string.size(), 0) !=
        static_cast<int>(gzipped_string.size())) {
      LOG(ERROR) << strerror(errno) << std::endl;
      return false;
    }

    return true;
  }

  bool WriteByParts(const Connection::Message& message) {
    String gzipped_string;
    {
      using namespace google::protobuf::io;

      StringOutputStream string_stream(&gzipped_string);
      GzipOutputStream::Options options;
      options.format = GzipOutputStream::ZLIB;
      GzipOutputStream gzip_stream(&string_stream, options);
      {
        CodedOutputStream coded_stream(&gzip_stream);
        coded_stream.WriteVarint32(message.ByteSize());
      }
      message.SerializePartialToZeroCopyStream(&gzip_stream);
    }

    CHECK(gzipped_string.size() < 128);
    for (size_t i = 0; i < gzipped_string.size(); ++i) {
      if (send(server_fd_, gzipped_string.data() + i, 1, 0) != 1) {
        LOG(ERROR) << strerror(errno) << std::endl;
        return false;
      }
    }

    return true;
  }

  bool WriteIncomplete(const Connection::Message& message) {
    String gzipped_string;
    {
      using namespace google::protobuf::io;

      StringOutputStream string_stream(&gzipped_string);
      GzipOutputStream::Options options;
      options.format = GzipOutputStream::ZLIB;
      GzipOutputStream gzip_stream(&string_stream, options);
      {
        CodedOutputStream coded_stream(&gzip_stream);
        coded_stream.WriteVarint32(message.ByteSize());
      }
      message.SerializePartialToZeroCopyStream(&gzip_stream);
    }

    CHECK(gzipped_string.size() / 2 < 128);
    if (send(server_fd_, gzipped_string.data(), gzipped_string.size() / 2, 0) !=
        static_cast<int>(gzipped_string.size() / 2)) {
      LOG(ERROR) << strerror(errno) << std::endl;
      return false;
    }

    return true;
  }

  bool ReadAtOnce(Connection::Message& message) {
    char buf[128];
    int size = recv(server_fd_, buf, 128, 0);
    if (size == 128) {
      LOG(ERROR) << "Incoming message is too big!" << std::endl;
      return false;
    }

    using namespace google::protobuf::io;

    ArrayInputStream array_stream(buf, size);
    GzipInputStream gzip_stream(&array_stream);
    ui32 message_size;
    {
      CodedInputStream coded_stream(&gzip_stream);
      coded_stream.ReadVarint32(&message_size);
    }
    message.ParsePartialFromBoundedZeroCopyStream(&gzip_stream, message_size);

    return true;
  }

 private:
  bool HandlePassive(Passive&& fd) override {
    // TODO: implement this.
    return false;
  }

  bool ReadyForRead(ConnectionImplPtr connection) override {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = connection.get();
    const auto& fd = connection->socket();
    if (epoll_ctl(epoll_fd_.native(), EPOLL_CTL_MOD, fd.native(), &event) ==
        -1) {
      DCHECK(errno == ENOENT);
      if (epoll_ctl(epoll_fd_.native(), EPOLL_CTL_ADD, fd.native(), &event) ==
          -1) {
        return false;
      }
    }
    return true;
  }

  bool ReadyForSend(ConnectionImplPtr connection) override {
    struct epoll_event event;
    event.events = EPOLLOUT | EPOLLONESHOT;
    event.data.ptr = connection.get();
    const auto& fd = connection->socket();
    if (epoll_ctl(epoll_fd_.native(), EPOLL_CTL_MOD, fd.native(), &event) ==
        -1) {
      DCHECK(errno == ENOENT);
      if (epoll_ctl(epoll_fd_.native(), EPOLL_CTL_ADD, fd.native(), &event) ==
          -1) {
        return false;
      }
    }
    return true;
  }

  void DoListenWork(const Atomic<bool>& is_shutting_down,
                    base::Data& self) override {
    // Test server doesn't do listening work.
  }

  void DoIOWork(const Atomic<bool>& is_shutting_down,
                base::Data& self) override {
    const int TIMEOUT = 1 * 1000;  // In milliseconds.
    std::array<struct epoll_event, 1> event;

    epoll_fd_.Add(self, EPOLLIN);

    while (!is_shutting_down) {
      auto events_count = epoll_fd_.Wait(event, TIMEOUT);
      if (events_count == -1) {
        if (errno != EINTR) {
          break;
        } else {
          continue;
        }
      }

      DCHECK(events_count == 1);
      auto* fd = reinterpret_cast<base::Handle*>(event[0].data.ptr);

      if (fd == &self) {
        continue;
      }

      auto ptr = reinterpret_cast<Connection*>(event[0].data.ptr);
      auto connection =
          std::static_pointer_cast<ConnectionImpl>(ptr->shared_from_this());

      if (event[0].events & (EPOLLHUP | EPOLLERR)) {
        DCHECK_O_EVAL(epoll_fd_.Delete(connection->socket()));
      }

      if (event[0].events & EPOLLIN) {
        ConnectionDoRead(connection);
      } else if (event[0].events & EPOLLOUT) {
        ConnectionDoSend(connection);
      }
    }
  }

  int listen_fd_, server_fd_;
  base::Epoll epoll_fd_;
  base::TemporaryDir tmp_dir_;
  String socket_path_;
};

class ConnectionTest : public ::testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(server.Init());
    connection = server.GetConnection();
    ASSERT_TRUE(!!connection);
  }
  void TearDown() override { connection.reset(); }

 protected:
  ConnectionImplPtr connection;
  TestServer server;
};

TEST_F(ConnectionTest, Sync_ReadOneMessage) {
  TestMessage test_message;

  auto expected_message = test_message.GetTestMessage();
  ASSERT_TRUE(server.WriteAtOnce(*expected_message));

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
  ASSERT_TRUE(server.WriteAtOnce(*expected_message1));
  ASSERT_TRUE(server.WriteAtOnce(*expected_message2));

  Connection::Message message;
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

  auto read_func = [&]() {
    Connection::Message message;
    proto::Status status;

    ASSERT_TRUE(connection->ReadSync(&message, &status))
        << status.description();

    EXPECT_EQ(proto::Status::OK, status.code());
    EXPECT_FALSE(status.has_description());
    test_message.CheckTestMessage(message);
  };

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);

  // FIXME: replace with |Promise|.
  std::this_thread::sleep_for(std::chrono::seconds(1));

  ASSERT_TRUE(server.WriteByParts(*expected_message));
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadUninitializedMessage) {
  const String expected_field2 = "arg2";
  const String expected_field3 = "arg3";

  Connection::Message expected_message;
  {
    auto message = expected_message.MutableExtension(proto::Test::extension);
    message->set_field2(expected_field2);
    message->add_field3()->assign(expected_field3);
  }
  ASSERT_TRUE(server.WriteAtOnce(expected_message));

  Connection::Message message;
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
  Connection::Message message;
  ASSERT_TRUE(server.ReadAtOnce(message));
  test_message.CheckTestMessage(message);
}

TEST_F(ConnectionTest, Sync_ReadIncompleteMessage) {
  TestMessage test_message;

  auto read_func = [&]() {
    Connection::Message message;
    proto::Status status;

    ASSERT_FALSE(connection->ReadSync(&message, &status));

    EXPECT_EQ(proto::Status::BAD_MESSAGE, status.code());
    EXPECT_TRUE(status.has_description());
  };

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);

  // FIXME: replace with |Promise|.
  std::this_thread::sleep_for(std::chrono::seconds(1));

  ASSERT_TRUE(server.WriteIncomplete(*expected_message));
  server.CloseServerConnection();
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_ReadFromClosedConnection) {
  Connection::Message message;
  proto::Status status;

  connection->Close();
  ASSERT_FALSE(connection->ReadSync(&message, &status));
  EXPECT_EQ(proto::Status::INCONSEQUENT, status.code());
  EXPECT_TRUE(status.has_description());
}

TEST_F(ConnectionTest, Sync_ReadAfterClosingConnectionOnServerSide) {
  TestMessage test_message;

  auto read_func = [&]() {
    Connection::Message message;
    proto::Status status;

    // FIXME: replace with |Promise|.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    ASSERT_TRUE(connection->ReadSync(&message, &status))
        << status.description();

    EXPECT_EQ(proto::Status::OK, status.code());
    EXPECT_FALSE(status.has_description());
    test_message.CheckTestMessage(message);
  };

  auto expected_message = test_message.GetTestMessage();
  std::thread read_thread(read_func);
  ASSERT_TRUE(server.WriteAtOnce(*expected_message));
  server.CloseServerConnection();
  read_thread.join();
}

TEST_F(ConnectionTest, Sync_SendToClosedConnection) {
  TestMessage message;
  auto expected_message = message.GetTestMessage();
  proto::Status status;

  connection->Close();
  ASSERT_FALSE(connection->SendSync(std::move(expected_message), &status));
  EXPECT_EQ(proto::Status::INCONSEQUENT, status.code());
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
  UniquePtr<proto::Test> expected_message;
  auto test_extension =
      test_message.GetTestMessage()->GetExtension(proto::Test::extension);
  expected_message.reset(new proto::Test(test_extension));
  proto::Status status;
  ASSERT_TRUE(connection->SendSync(std::move(expected_message), &status))
      << status.description();
  Connection::Message message;
  ASSERT_TRUE(server.ReadAtOnce(message));
  test_message.CheckTestMessage(message);
}

}  // namespace testing
}  // namespace dist_clang
