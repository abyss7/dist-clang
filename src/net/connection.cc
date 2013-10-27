#include "net/connection.h"

#include "base/assert.h"
#include "net/base/utils.h"
#include "net/event_loop.h"

#include <cstdint>
#include <iostream>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

// static
ConnectionPtr Connection::Create(EventLoop &event_loop, fd_t fd,
                                 const EndPointPtr& end_point) {
#if !defined(OS_MACOSX)
  DCHECK(!IsListening(fd));
#endif
  DCHECK(!IsNonBlocking(fd));
  return ConnectionPtr(new Connection(event_loop, fd, end_point));
}

Connection::Connection(EventLoop& event_loop, fd_t fd,
                       const EndPointPtr& end_point)
  : fd_(fd), event_loop_(event_loop), is_closed_(false), added_(false),
    end_point_(end_point), file_input_stream_(fd_, 1024),
    file_output_stream_(fd_, 1024) {
}

Connection::~Connection() {
  Close();
}

bool Connection::ReadAsync(ReadCallback callback) {
  read_callback_ = std::bind(callback, shared_from_this(), _1, _2);
  if (!event_loop_.ReadyForRead(shared_from_this())) {
    read_callback_ = BindedReadCallback();
    return false;
  }
  return true;
}

template <>
bool Connection::SendAsync(unique_ptr<Message> message, SendCallback callback) {
  message_ = std::move(message);
  return SendAsync(callback);
}

bool Connection::ReadSync(Message *message, Status *status) {
  if (!message) {
    if (status) {
      status->set_code(Status::EMPTY_MESSAGE);
      status->set_description("Message pointer is null");
    }
    return false;
  }

  unsigned size;
  {
    CodedInputStream coded_stream(&file_input_stream_);
    if (!coded_stream.ReadVarint32(&size)) {
      if (file_input_stream_.GetErrno() && status) {
        status->set_code(Status::NETWORK);
        status->set_description("Can't read incoming message size: ");
        status->mutable_description()->append(
            strerror(file_input_stream_.GetErrno()));
      }
      return false;
    }
  }

  if (!size) {
    if (status) {
      status->set_code(Status::BAD_MESSAGE);
      status->set_description("Incoming message has zero size");
    }
    return false;
  }

  if (!message->ParseFromBoundedZeroCopyStream(&file_input_stream_, size)) {
    if (status) {
      status->set_code(Status::BAD_MESSAGE);
      status->set_description("Incoming message is malformed");
      if (file_input_stream_.GetErrno()) {
        status->mutable_description()->append(": ");
        status->mutable_description()->append(
          strerror(file_input_stream_.GetErrno()));
      }
    }
    return false;
  }

  return true;
}

template <>
bool Connection::SendSync(unique_ptr<Message> message, Status* status) {
  message_ = std::move(message);
  return SendSync(status);
}

bool Connection::SendAsync(SendCallback callback) {
  send_callback_ = std::bind(callback, shared_from_this(), _1);
  if (!event_loop_.ReadyForSend(shared_from_this())) {
    send_callback_ = BindedSendCallback();
    return false;
  }
  return true;
}

bool Connection::SendSync(Status* status) {
  {
    CodedOutputStream coded_stream(&file_output_stream_);
    coded_stream.WriteVarint32(message_->ByteSize());
    if (!message_->SerializeToCodedStream(&coded_stream)) {
      if (status) {
        status->set_code(Status::NETWORK);
        status->set_description("Can't serialize message to coded stream");
        if (coded_stream.HadError()) {
          auto* description = status->mutable_description();
          description->append(": ");
          description->append(strerror(file_output_stream_.GetErrno()));
        }
      }
      return false;
    }
  }

  // The method |Flush()| calls function |write()| and potentially can raise
  // the signal |SIGPIPE|.
  if (!file_output_stream_.Flush()) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't flush sent message to socket");
    }
    return false;
  }

  return true;
}

bool Connection::ReportStatus(const Status& message, SendCallback callback) {
  message_.reset(new Message);
  message_->SetAllocatedExtension(Status::extension, new Status(message));
  send_callback_ = std::bind(callback, shared_from_this(), _1);
  if (!event_loop_.ReadyForSend(shared_from_this())) {
    send_callback_ = BindedSendCallback();
    return false;
  }
  return true;
}

// static
Connection::SendCallback Connection::CloseAfterSend() {
  auto callback = [](ConnectionPtr, const Status& status) -> bool {
    if (status.code() != Status::OK) {
      std::cerr << "Failed to send message: " << status.description()
                << std::endl;
    }
    return false;
  };
  return std::bind(callback, _1, _2);
}

void Connection::DoRead() {
  Status status;
  message_.reset(new Message);
  ReadSync(message_.get(), &status);
  DCHECK(!!read_callback_);
  auto read_callback = read_callback_;
  read_callback_ = BindedReadCallback();
  if (!read_callback(std::move(message_), status)) {
    Close();
  }
}

void Connection::DoSend() {
  Status status;
  SendSync(std::move(message_), &status);
  DCHECK(!!send_callback_);
  auto send_callback = send_callback_;
  send_callback_ = BindedSendCallback();
  if (!send_callback(status)) {
    Close();
  }
}

void Connection::Close() {
  bool old_closed = false;
  if (is_closed_.compare_exchange_strong(old_closed, true)) {
    read_callback_ = BindedReadCallback();
    send_callback_ = BindedSendCallback();
    // TODO: do the "polite" shutdown.
    close(fd_);
  }
}

}  // namespace net
}  // namespace dist_clang
