#pragma once

#include "net/base/types.h"
#include "net/connection_forward.h"
#include "proto/remote.pb.h"

#include <atomic>
#include <functional>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

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
    void Close();
    bool IsClosed() const;
    bool IsOnEventLoop(const EventLoop* event_loop) const;

  private:
    friend class EventLoop;

    enum State { IDLE, WAITING_SEND, WAITING_READ, SENDING, READING };

    Connection(EventLoop& event_loop, fd_t fd);

    void CanRead();
    void CanSend();

    const fd_t fd_;
    EventLoop& event_loop_;
    volatile bool is_closed_;
    std::atomic<State> state_;

    // Read members.
    FileInputStream file_input_stream_;
    std::unique_ptr<CodedInputStream> coded_input_stream_;
    ReadCallback read_callback_;
    Message input_message_;
    Limit input_limit_;

    // Send members.
    FileOutputStream file_output_stream_;
    std::unique_ptr<CodedOutputStream> coded_output_stream_;
    SendCallback send_callback_;
    size_t total_sent_;
    std::string output_message_;
};

}  // namespace net
}  // namespace dist_clang
