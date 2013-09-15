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
  public:
    typedef proto::Universal Message;
    typedef proto::Error Error;
    typedef google::protobuf::io::FileInputStream FileInputStream;
    typedef google::protobuf::io::FileOutputStream FileOutputStream;
    typedef google::protobuf::io::CodedInputStream CodedInputStream;
    typedef google::protobuf::io::CodedOutputStream CodedOutputStream;
    typedef google::protobuf::io::CodedInputStream::Limit Limit;
    typedef std::function<bool(ConnectionPtr, const Message&, const Error&)>
        ReadCallback;
    typedef std::function<bool(ConnectionPtr, const Error&)> SendCallback;

    // Create connection only on an active socket -
    // i.e. after connect() or accept().
    static ConnectionPtr Create(EventLoop& event_loop, fd_t fd);
    ~Connection();

    // Asynchronous functions return |false| in case of inconsequent call.
    // The connection is not closed in this case.
    bool Read(ReadCallback callback);
    bool Send(const Message& message, SendCallback callback);

    bool Read(Message* message, Error* error);
    bool Send(const Message& message, Error* error);
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
    Message output_message_;
};

}  // namespace net
}  // namespace dist_clang
