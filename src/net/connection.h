#pragma once

#include "net/base/types.h"
#include "net/connection_forward.h"
#include "proto/remote.pb.h"

#include <atomic>
#include <functional>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <unistd.h>

namespace dist_clang {
namespace net {

class EventLoop;

class Connection: public std::enable_shared_from_this<Connection> {
    using CodedInputStream = google::protobuf::io::CodedInputStream;
    using CodedOutputStream = google::protobuf::io::CodedOutputStream;
    using CustomMessage = google::protobuf::Message;
    using FileInputStream = google::protobuf::io::FileInputStream;
    using FileOutputStream = google::protobuf::io::FileOutputStream;
    using GzipInputStream = google::protobuf::io::GzipInputStream;
    using GzipOutputStream = google::protobuf::io::GzipOutputStream;
    using Limit = google::protobuf::io::CodedInputStream::Limit;
    using Status = proto::Status;

  public:
    using Message = proto::Universal;
    using ReadCallback =
        std::function<bool(ConnectionPtr, const Message&, const Status&)>;
    using SendCallback = std::function<bool(ConnectionPtr, const Status&)>;

    // Create connection only on a socket with a pending connection -
    // i.e. after connect() or accept().
    static ConnectionPtr Create(EventLoop& event_loop, fd_t fd,
                                const EndPointPtr& end_point = EndPointPtr());
    ~Connection();

    bool ReadAsync(ReadCallback callback);
    bool SendAsync(const CustomMessage& message,
                   SendCallback callback = CloseAfterSend());
    static SendCallback CloseAfterSend();

    bool ReadSync(Message* message, Status* status = nullptr);
    bool SendSync(const CustomMessage& message, Status* status = nullptr);
    inline bool IsOnEventLoop(const EventLoop* event_loop) const;

  private:
    friend class EventLoop;

    using BindedReadCallback =
        std::function<bool(const Message&, const Status&)>;
    using BindedSendCallback = std::function<bool(const Status&)>;

    enum State { IDLE, WAITING_SEND, WAITING_READ, SENDING, READING };

    Connection(EventLoop& event_loop, fd_t fd, const EndPointPtr& end_point);

    void DoRead();
    void DoSend();
    void Close();
    inline bool AddToEventLoop();
    bool ConvertCustomMessage(const CustomMessage& input, Message* output,
                              Status* status = nullptr);

    const fd_t fd_;
    Message message_;
    EventLoop& event_loop_;
    std::atomic<bool> is_closed_, added_;
    EndPointPtr end_point_;

    // Read members.
    FileInputStream file_input_stream_;
    BindedReadCallback read_callback_;

    // Send members.
    FileOutputStream file_output_stream_;
    BindedSendCallback send_callback_;
};

bool Connection::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

bool Connection::AddToEventLoop() {
  bool old_added = false;
  return added_.compare_exchange_strong(old_added, true);
}

}  // namespace net
}  // namespace dist_clang
