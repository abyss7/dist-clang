#pragma once

#include <histogram/counter.h>
#include <net/base/types.h>
#include <net/connection.h>

#include <third_party/gtest/public/gtest/gtest_prod.h>
#include <third_party/libcxx/exported/include/atomic>
#include <third_party/protobuf/exported/google/protobuf/io/coded_stream.h>
#include <third_party/protobuf/exported/google/protobuf/io/gzip_stream.h>
#include <third_party/protobuf/exported/google/protobuf/io/zero_copy_stream_impl.h>

#include <unistd.h>

namespace dist_clang {
namespace net {

class EventLoop;

class ConnectionImpl : public Connection {
 public:
  // Create connection only on a socket with a pending connection -
  // i.e. after connect() or accept().
  static ConnectionImplPtr Create(EventLoop& event_loop, fd_t fd,
                                  const EndPointPtr& end_point = EndPointPtr());
  ~ConnectionImpl();

  virtual bool ReadAsync(ReadCallback callback) override;
  virtual bool ReadSync(Message* message, Status* status = nullptr) override;

  using Connection::SendSync;

  inline bool IsOnEventLoop(const EventLoop* event_loop) const;

 private:
  friend class EventLoop;

  using CodedInputStream = google::protobuf::io::CodedInputStream;
  using CodedOutputStream = google::protobuf::io::CodedOutputStream;
  using FileInputStream = google::protobuf::io::FileInputStream;
  using FileOutputStream = google::protobuf::io::FileOutputStream;
  using GzipInputStream = google::protobuf::io::GzipInputStream;
  using GzipOutputStream = google::protobuf::io::GzipOutputStream;
  using BindedReadCallback = Fn<bool(ScopedMessage, const Status&)>;
  using BindedSendCallback = Fn<bool(const Status&)>;

  enum State {
    IDLE,
    WAITING_SEND,
    WAITING_READ,
    SENDING,
    READING,
  };

  // FIXME: make this value configurable.
  enum : ui32 { buffer_size = 1024 };

  ConnectionImpl(EventLoop& event_loop, fd_t fd, const EndPointPtr& end_point);

  virtual bool SendAsyncImpl(SendCallback callback) override;
  virtual bool SendSyncImpl(Status* status) override;

  void DoRead();
  void DoSend();
  void Close();

  const fd_t fd_;

  EventLoop& event_loop_;
  std::atomic<bool> is_closed_, added_;
  EndPointPtr end_point_;

  // Read members.
  FileInputStream file_input_stream_;
  UniquePtr<GzipInputStream> gzip_input_stream_;
  BindedReadCallback read_callback_;

  // Send members.
  FileOutputStream file_output_stream_;
  UniquePtr<GzipOutputStream> gzip_output_stream_;
  BindedSendCallback send_callback_;

  FRIEND_TEST(ConnectionTest, Sync_ReadFromClosedConnection);
  FRIEND_TEST(ConnectionTest, Sync_ReadIncompleteMessage);
  FRIEND_TEST(ConnectionTest, Sync_SendToClosedConnection);
};

bool ConnectionImpl::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

}  // namespace net
}  // namespace dist_clang
