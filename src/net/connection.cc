#include "net/connection.h"

#include "net/base/utils.h"
#include "net/event_loop.h"

#include <cstdint>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

// static
ConnectionPtr Connection::Create(EventLoop &event_loop, fd_t fd) {
  assert(IsNonBlocking(fd));
  return ConnectionPtr(new Connection(event_loop, fd));
}

Connection::Connection(EventLoop& event_loop, fd_t fd)
  : fd_(fd), event_loop_(event_loop), is_closed_(false), state_(IDLE),
    file_input_stream_(fd_, 1024), input_limit_(0), file_output_stream_(fd_) {}

Connection::~Connection() {
  Close();
}

bool Connection::ReadAsync(ReadCallback callback, Status* status) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_READ)) {
    if (status) {
      status->set_code(Status::INCONSEQUENT);
      status->set_description("Trying to read while doing another operation");
    }
    return false;
  }

  read_callback_ = callback;
  event_loop_.ReadyForRead(shared_from_this());
  return true;
}

bool Connection::SendAsync(const CustomMessage &message, SendCallback callback,
                           Status* status) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_SEND)) {
    if (status) {
      status->set_code(Status::INCONSEQUENT);
      status->set_description("Trying to send while doing another operation");
    }
    return false;
  }

  // Handle |message|.
  proto::Universal output_message;
  if (message.GetTypeName() == output_message.GetTypeName())
    output_message.CopyFrom(message);
  else {
    output_message.Clear();
    const google::protobuf::FieldDescriptor* extension_field = nullptr;
    auto descriptor = message.GetDescriptor();
    auto reflection = output_message.GetReflection();
    for (int i = 0; i < descriptor->extension_count(); ++i) {
      extension_field = reflection->FindKnownExtensionByName(
          descriptor->extension(i)->full_name());
      if (extension_field)
        break;
    }
    if (!extension_field) {
      if (status) {
        status->set_code(Status::EMPTY_MESSAGE);
        status->set_description("Message of type " + message.GetTypeName() +
                               " can't be sent, since it doesn't extend " +
                               output_message.GetTypeName());
      }
      return false;
    }
    reflection->MutableMessage(&output_message, extension_field)->
        CopyFrom(message);
  }

  output_message.SerializeToString(&output_message_);
  send_callback_ = callback;
  event_loop_.ReadyForSend(shared_from_this());
  return true;
}

// static
Connection::SendCallback Connection::CloseAfterSend() {
  auto callback = [](ConnectionPtr, const Status&) -> bool { return false; };
  return std::bind(callback, _1, _2);
}

// static
Connection::SendCallback Connection::Idle() {
  auto callback = [](ConnectionPtr, const Status&) -> bool { return true; };
  return std::bind(callback, _1, _2);
}

bool Connection::ReadSync(Message *message, Status *status) {
  if (!message) {
    if (status) {
      status->set_code(Status::EMPTY_MESSAGE);
      status->set_description("Message pointer is null");
    }
    return false;
  }

  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, READING)) {
    if (status) {
      status->set_code(Status::INCONSEQUENT);
      status->set_description("Can't read synchronously while other operation "
                             "is in progress");
    }
    return false;
  }

  message->Clear();
  if (!MakeNonBlocking(fd_, true)) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't make socket a blocking one");
    }
    return false;
  }

  coded_input_stream_.reset(new CodedInputStream(&file_input_stream_));

  unsigned message_size;
  if (!coded_input_stream_->ReadVarint32(&message_size)) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't read incoming message size: ");
      status->mutable_description()->append(
          strerror(file_input_stream_.GetErrno()));
    }
    Close();
    return false;
  }
  input_limit_ = coded_input_stream_->PushLimit(message_size);

  while (coded_input_stream_->BytesUntilLimit() > 0 &&
         !coded_input_stream_->ExpectAtEnd())
    message->MergePartialFromCodedStream(coded_input_stream_.get());

  if (coded_input_stream_->BytesUntilLimit() != 0 ||
      !message->IsInitialized() ||
      !coded_input_stream_->ConsumedEntireMessage()) {
    if (status) {
      status->set_code(Status::BAD_MESSAGE);
      status->set_description("Incoming message is malformed");
    }
    Close();
    return false;
  }
  coded_input_stream_->PopLimit(input_limit_);
  coded_input_stream_.reset();

  if (!MakeNonBlocking(fd_, false)) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't make socket a non-blocking one");
    }
    return false;
  }

  state_.store(IDLE);
  return true;
}

bool Connection::SendSync(const CustomMessage &message, Status *status) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, SENDING)) {
    if (status) {
      status->set_code(Status::INCONSEQUENT);
      status->set_description("Can't send synchronously while other operation "
                             "is in progress");
    }
    return false;
  }

  // Handle |message|.
  proto::Universal output_message;
  if (message.GetTypeName() == output_message.GetTypeName())
    output_message.CopyFrom(message);
  else {
    output_message.Clear();
    const google::protobuf::FieldDescriptor* extension_field = nullptr;
    auto descriptor = message.GetDescriptor();
    auto reflection = output_message.GetReflection();
    for (int i = 0; i < descriptor->extension_count(); ++i) {
      extension_field = reflection->FindKnownExtensionByName(
          descriptor->extension(i)->full_name());
      if (extension_field)
        break;
    }
    if (!extension_field) {
      if (status) {
        status->set_code(Status::EMPTY_MESSAGE);
        status->set_description("Message of type " + message.GetTypeName() +
                               " can't be sent, since it doesn't extend " +
                               output_message.GetTypeName());
      }
      return false;
    }
    reflection->MutableMessage(&output_message, extension_field)->
        CopyFrom(message);
  }

  if (!MakeNonBlocking(fd_, true)) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't make socket a blocking one");
    }
    return false;
  }

  coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));

  coded_output_stream_->WriteVarint32(output_message.ByteSize());
  auto sent_bytes = coded_output_stream_->ByteCount();
  if (!output_message.SerializeToCodedStream(coded_output_stream_.get()) ||
      coded_output_stream_->ByteCount() != sent_bytes +
                                           output_message.ByteSize()) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't send whole message at once");
    }
    Close();
    return false;
  }

  coded_output_stream_.reset();
  file_output_stream_.Flush();

  if (!MakeNonBlocking(fd_, false)) {
    if (status) {
      status->set_code(Status::NETWORK);
      status->set_description("Can't make socket a non-blocking one");
    }
    return false;
  }

  state_.store(IDLE);
  return true;
}

void Connection::CanRead() {
  Status status;
  assert(state_.load() == WAITING_READ || state_.load() == READING);

  if (state_.load() == WAITING_READ) {
    input_message_.Clear();
    unsigned message_size;

    coded_input_stream_.reset(new CodedInputStream(&file_input_stream_));

    if (!coded_input_stream_->ReadVarint32(&message_size)) {
      status.set_code(Status::NETWORK);
      status.set_description("Can't read incoming message size");
      if (!read_callback_(shared_from_this(), input_message_, status))
        Close();
      return;
    }
    input_limit_ = coded_input_stream_->PushLimit(message_size);
    state_.store(READING);
  }

  input_message_.MergePartialFromCodedStream(coded_input_stream_.get());
  if (coded_input_stream_->BytesUntilLimit() > 0) {
    event_loop_.ReadyForRead(shared_from_this());
    return;
  }
  else if (!input_message_.IsInitialized() ||
           !coded_input_stream_->ConsumedEntireMessage()) {
    status.set_code(Status::BAD_MESSAGE);
    status.set_description("Incoming message is malformed");
    if (!read_callback_(shared_from_this(), input_message_, status))
      Close();
    return;
  }
  coded_input_stream_->PopLimit(input_limit_);
  coded_input_stream_.reset();
  state_.store(IDLE);
  if (!read_callback_(shared_from_this(), input_message_, status))
    Close();
}

void Connection::CanSend() {
  Status status;
  assert(state_.load() == WAITING_SEND || state_.load() == SENDING);

  if (state_.load() == WAITING_SEND) {
    coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));
    coded_output_stream_->WriteVarint32(output_message_.size());
    coded_output_stream_.reset();
    file_output_stream_.Flush();
    total_sent_ = 0;
    state_.store(SENDING);
  }

  auto sent = write(fd_, output_message_.data() + total_sent_,
                    output_message_.size() - total_sent_);
  if (sent <= 0) {
    status.set_code(Status::NETWORK);
    status.set_description("Can't send message");
    if (!send_callback_(shared_from_this(), status))
      Close();
    return;
  }
  total_sent_ += sent;

  if (total_sent_ < output_message_.size()) {
    event_loop_.ReadyForSend(shared_from_this());
    return;
  }

  state_.store(IDLE);
  if (!send_callback_(shared_from_this(), status))
    Close();
}

void Connection::Close() {
  if (!is_closed_) {
    is_closed_ = true;
    close(fd_);
  }
}

bool Connection::IsClosed() const {
  return is_closed_;
}

bool Connection::IsOnEventLoop(const EventLoop* event_loop) const {
  return &event_loop_ == event_loop;
}

}  // namespace net
}  // namespace dist_clang
