#pragma once

#include "net/base/types.h"
#include "net/connection_forward.h"
#include "proto/remote.pb.h"

#include <atomic>
#include <functional>
#include <google/protobuf/io/coded_stream.h>
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
    using Limit = google::protobuf::io::CodedInputStream::Limit;
    using Status = proto::Status;

  public:
    using Message = proto::Universal;
    using ReadCallback =
        std::function<bool(ConnectionPtr, const Message&, const Status&)>;
    using SendCallback = std::function<bool(ConnectionPtr, const Status&)>;

    // Create connection only on an active socket -
    // i.e. after connect() or accept().
    static ConnectionPtr Create(EventLoop& event_loop, fd_t fd);
    ~Connection();

    bool ReadAsync(ReadCallback callback, Status* status = nullptr);
    bool SendAsync(const CustomMessage& message,
                   SendCallback callback = Idle(),
                   Status* status = nullptr);
    static SendCallback CloseAfterSend();
    static SendCallback Idle();

    bool ReadSync(Message* message, Status* status = nullptr);
    bool SendSync(const CustomMessage& message, Status* status = nullptr);
    inline void Close();
    inline bool IsOnEventLoop(const EventLoop* event_loop) const;

  private:
    friend class EventLoop;

    enum State { IDLE, WAITING_SEND, WAITING_READ, SENDING, READING };

    Connection(EventLoop& event_loop, fd_t fd);

    void DoRead();
    void DoSend();
    inline bool ToggleWait(bool new_wait);
    bool ConvertCustomMessage(const CustomMessage& input, Message* output,
                              Status* status = nullptr);

    const fd_t fd_;
    Message message_;
    EventLoop& event_loop_;
    std::atomic<bool> is_closed_, waiting_;

    // Read members.
    FileInputStream file_input_stream_;
    ReadCallback read_callback_;

    // Send members.
    FileOutputStream file_output_stream_;
    SendCallback send_callback_;
};

void Connection::Close() {
  bool old_closed = false;
  if (is_closed_.compare_exchange_strong(old_closed, true)) {
    close(fd_);
  }
}

bool Connection::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

bool Connection::ToggleWait(bool new_wait) {
  bool old_wait = !new_wait;
  return waiting_.compare_exchange_strong(old_wait, new_wait);
}

}  // namespace net
}  // namespace dist_clang
