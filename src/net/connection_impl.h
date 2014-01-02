#pragma once

#include "gtest/gtest_prod.h"
#include "net/base/types.h"
#include "net/connection.h"
#include "proto/remote.pb.h"

#include <atomic>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <unistd.h>

namespace dist_clang {
namespace net {

class EventLoop;

class ConnectionImpl: public Connection,
                      public std::enable_shared_from_this<ConnectionImpl> {
  public:
    // Create connection only on a socket with a pending connection -
    // i.e. after connect() or accept().
    static ConnectionPtr Create(EventLoop& event_loop, fd_t fd,
                                const EndPointPtr& end_point = EndPointPtr());
    ~ConnectionImpl();

    virtual bool ReadAsync(ReadCallback callback) override;
    virtual bool ReadSync(Message* message, Status* status = nullptr) override;

    template <class M>
    bool SendAsync(
        UniquePtr<M> message, SendCallback callback = CloseAfterSend());

    template <class M>
    bool SendSync(UniquePtr<M> message, Status* status = nullptr);

    bool ReportStatus(
        const Status& message, SendCallback callback = CloseAfterSend());

    static SendCallback CloseAfterSend();

    inline bool IsOnEventLoop(const EventLoop* event_loop) const;

  private:
    friend class EventLoop;

    using CodedInputStream = google::protobuf::io::CodedInputStream;
    using CodedOutputStream = google::protobuf::io::CodedOutputStream;
    using FileInputStream = google::protobuf::io::FileInputStream;
    using FileOutputStream = google::protobuf::io::FileOutputStream;
    using BindedReadCallback = Fn<bool(ScopedMessage, const Status&)>;
    using BindedSendCallback = Fn<bool(const Status&)>;

    enum State { IDLE, WAITING_SEND, WAITING_READ, SENDING, READING };

    ConnectionImpl(EventLoop& event_loop, fd_t fd,
                   const EndPointPtr& end_point);

    virtual bool SendAsync(SendCallback callback) override;
    virtual bool SendSync(Status* status) override;

    void DoRead();
    void DoSend();
    void Close();

    const fd_t fd_;

    EventLoop& event_loop_;
    std::atomic<bool> is_closed_, added_;
    EndPointPtr end_point_;

    // Read members.
    FileInputStream file_input_stream_;
    BindedReadCallback read_callback_;

    // Send members.
    FileOutputStream file_output_stream_;
    BindedSendCallback send_callback_;

    FRIEND_TEST(ConnectionTest, Sync_ReadFromClosedConnection);
    FRIEND_TEST(ConnectionTest, Sync_ReadIncompleteMessage);
    FRIEND_TEST(ConnectionTest, Sync_SendToClosedConnection);
};

template <class M>
bool ConnectionImpl::SendAsync(UniquePtr<M> message, SendCallback callback) {
  message_.reset(new Message);
  message_->SetAllocatedExtension(M::extension, message.release());
  return SendAsync(callback);
}

template <>
bool ConnectionImpl::SendAsync(ScopedMessage message, SendCallback callback);

template <class M>
bool ConnectionImpl::SendSync(UniquePtr<M> message, Status* status) {
  message_.reset(new Message);
  message_->SetAllocatedExtension(M::extension, message.release());
  return SendSync(status);
}

template <>
bool ConnectionImpl::SendSync(ScopedMessage message, Status* status);

bool ConnectionImpl::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

}  // namespace net
}  // namespace dist_clang
