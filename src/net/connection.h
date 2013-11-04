#pragma once

#include "net/base/types.h"
#include "net/connection_forward.h"
#include "net/socket_output_stream.h"
#include "proto/remote.pb.h"

#include <atomic>
#include <functional>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <gtest/gtest_prod.h>
#include <memory>

#include <unistd.h>

namespace dist_clang {
namespace net {

class EventLoop;

class Connection: public std::enable_shared_from_this<Connection> {
    using Status = proto::Status;

    template <class T>
    using unique_ptr = std::unique_ptr<T>;
    template <class Signature>
    using function = std::function<Signature>;

  public:
    using Message = proto::Universal;
    using ScopedMessage = unique_ptr<Message>;
    using ReadCallback =
        function<bool(ConnectionPtr, ScopedMessage, const Status&)>;
    using SendCallback = function<bool(ConnectionPtr, const Status&)>;

    // Create connection only on a socket with a pending connection -
    // i.e. after connect() or accept().
    static ConnectionPtr Create(EventLoop& event_loop, fd_t fd,
                                const EndPointPtr& end_point = EndPointPtr());
    ~Connection();

    bool ReadAsync(ReadCallback callback);
    template <class M>
    bool SendAsync(unique_ptr<M> message,
                   SendCallback callback = CloseAfterSend());

    bool ReadSync(Message* message, Status* status = nullptr);
    template <class M>
    bool SendSync(unique_ptr<M> message, Status* status = nullptr);

    bool ReportStatus(const Status& message,
                      SendCallback callback = CloseAfterSend());
    static SendCallback CloseAfterSend();
    inline bool IsOnEventLoop(const EventLoop* event_loop) const;

  private:
    friend class EventLoop;

    using BindedReadCallback = function<bool(ScopedMessage, const Status&)>;
    using BindedSendCallback = function<bool(const Status&)>;
    using CodedInputStream = google::protobuf::io::CodedInputStream;
    using CodedOutputStream = google::protobuf::io::CodedOutputStream;
    using FileInputStream = google::protobuf::io::FileInputStream;

    enum State { IDLE, WAITING_SEND, WAITING_READ, SENDING, READING };

    Connection(EventLoop& event_loop, fd_t fd, const EndPointPtr& end_point);

    void DoRead();
    void DoSend();
    void Close();
    bool SendAsync(SendCallback callback);
    bool SendSync(Status* status);

    const fd_t fd_;
    ScopedMessage message_;
    EventLoop& event_loop_;
    std::atomic<bool> is_closed_;

    // Read members.
    FileInputStream file_input_stream_;
    BindedReadCallback read_callback_;

    // Send members.
    SocketOutputStream file_output_stream_;
    BindedSendCallback send_callback_;

    FRIEND_TEST(ConnectionTest, Sync_ReadFromClosedConnection);
    FRIEND_TEST(ConnectionTest, Sync_ReadIncompleteMessage);
    FRIEND_TEST(ConnectionTest, Sync_SendToClosedConnection);
};

template <class M>
bool Connection::SendAsync(unique_ptr<M> message, SendCallback callback) {
  message_.reset(new Message);
  message_->SetAllocatedExtension(M::extension, message.release());
  return SendAsync(callback);
}

template <>
bool Connection::SendAsync(unique_ptr<Message> message, SendCallback callback);

template <class M>
bool Connection::SendSync(unique_ptr<M> message, Status* status) {
  message_.reset(new Message);
  message_->SetAllocatedExtension(M::extension, message.release());
  return SendSync(status);
}

template <>
bool Connection::SendSync(unique_ptr<Message> message, Status* status);

bool Connection::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

}  // namespace net
}  // namespace dist_clang
