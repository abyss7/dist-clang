#include "net/connection.h"

#include "net/base/utils.h"
#include "net/event_loop.h"

#include <cstdint>

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

bool Connection::ReadAsync(ReadCallback callback, Error* error) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_READ)) {
    if (error) {
      error->set_code(Error::INCONSEQUENT);
      error->set_description("Trying to read while doing another operation");
    }
    return false;
  }

  read_callback_ = callback;
  event_loop_.ReadyForRead(shared_from_this());
  return true;
}

bool Connection::SendAsync(const CustomMessage &message, SendCallback callback,
                           Error* error) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_SEND)) {
    if (error) {
      error->set_code(Error::INCONSEQUENT);
      error->set_description("Trying to send while doing another operation");
    }
    return false;
  }

  // Handle |message|.
  if (message.GetTypeName() == output_message_.GetTypeName())
    output_message_.CopyFrom(message);
  else {
    output_message_.Clear();
    const google::protobuf::FieldDescriptor* extension_field = nullptr;
    auto descriptor = message.GetDescriptor();
    auto reflection = output_message_.GetReflection();
    for (int i = 0; i < descriptor->extension_count(); ++i) {
      extension_field = reflection->FindKnownExtensionByName(
          descriptor->extension(i)->full_name());
      if (extension_field)
        break;
    }
    if (!extension_field) {
      if (error) {
        error->set_code(Error::EMPTY_MESSAGE);
        error->set_description("Message of type " + message.GetTypeName() +
                               " can't be sent, since it doesn't extend " +
                               output_message_.GetTypeName());
      }
      return false;
    }
    reflection->MutableMessage(&output_message_, extension_field)->
        CopyFrom(message);
  }

  send_callback_ = callback;
  event_loop_.ReadyForSend(shared_from_this());
  return true;
}

// static
Connection::SendCallback Connection::CloseAfterSend() {
  using namespace std::placeholders;
  auto callback = [](ConnectionPtr, const Error&) -> bool { return false; };
  return std::bind(callback, _1, _2);
}

// static
Connection::SendCallback Connection::Idle() {
  using namespace std::placeholders;
  auto callback = [](ConnectionPtr, const Error&) -> bool { return true; };
  return std::bind(callback, _1, _2);
}

bool Connection::ReadSync(Message *message, Error *error) {
  if (!message) {
    if (error) {
      error->set_code(Error::EMPTY_MESSAGE);
      error->set_description("Message pointer is null");
    }
    return false;
  }

  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, READING)) {
    if (error) {
      error->set_code(Error::INCONSEQUENT);
      error->set_description("Can't read synchronously while other operation "
                             "is in progress");
    }
    return false;
  }

  message->Clear();
  if (!MakeNonBlocking(fd_, true)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't make socket a blocking one");
    }
    return false;
  }

  coded_input_stream_.reset(new CodedInputStream(&file_input_stream_));

  unsigned message_size;
  if (!coded_input_stream_->ReadVarint32(&message_size)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't read incoming message size: ");
      error->mutable_description()->append(
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
    if (error) {
      error->set_code(Error::BAD_MESSAGE);
      error->set_description("Incoming message is malformed");
    }
    Close();
    return false;
  }
  coded_input_stream_->PopLimit(input_limit_);
  coded_input_stream_.reset();

  if (!MakeNonBlocking(fd_, false)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't make socket a non-blocking one");
    }
    return false;
  }

  state_.store(IDLE);
  return true;
}

bool Connection::SendSync(const CustomMessage &message, Error *error) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, SENDING)) {
    if (error) {
      error->set_code(Error::INCONSEQUENT);
      error->set_description("Can't send synchronously while other operation "
                             "is in progress");
    }
    return false;
  }

  // Handle |message|.
  if (message.GetTypeName() == output_message_.GetTypeName())
    output_message_.CopyFrom(message);
  else {
    output_message_.Clear();
    const google::protobuf::FieldDescriptor* extension_field = nullptr;
    auto descriptor = message.GetDescriptor();
    auto reflection = output_message_.GetReflection();
    for (int i = 0; i < descriptor->extension_count(); ++i) {
      extension_field = reflection->FindKnownExtensionByName(
          descriptor->extension(i)->full_name());
      if (extension_field)
        break;
    }
    if (!extension_field) {
      if (error) {
        error->set_code(Error::EMPTY_MESSAGE);
        error->set_description("Message of type " + message.GetTypeName() +
                               " can't be sent, since it doesn't extend " +
                               output_message_.GetTypeName());
      }
      return false;
    }
    reflection->MutableMessage(&output_message_, extension_field)->
        CopyFrom(message);
  }

  if (!MakeNonBlocking(fd_, true)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't make socket a blocking one");
    }
    return false;
  }

  coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));

  coded_output_stream_->WriteVarint32(output_message_.ByteSize());
  auto sent_bytes = coded_output_stream_->ByteCount();
  if (!output_message_.SerializeToCodedStream(coded_output_stream_.get()) ||
      coded_output_stream_->ByteCount() != sent_bytes +
                                           output_message_.ByteSize()) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't send whole message at once");
    }
    Close();
    return false;
  }

  coded_output_stream_.reset();
  file_output_stream_.Flush();

  if (!MakeNonBlocking(fd_, false)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't make socket a non-blocking one");
    }
    return false;
  }

  state_.store(IDLE);
  return true;
}

void Connection::CanRead() {
  Error error;
  assert(state_.load() == WAITING_READ || state_.load() == READING);

  if (state_.load() == WAITING_READ) {
    input_message_.Clear();
    unsigned message_size;

    coded_input_stream_.reset(new CodedInputStream(&file_input_stream_));

    if (!coded_input_stream_->ReadVarint32(&message_size)) {
      error.set_code(Error::NETWORK);
      error.set_description("Can't read incoming message size");
      if (!read_callback_(shared_from_this(), input_message_, error))
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
    error.set_code(Error::BAD_MESSAGE);
    error.set_description("Incoming message is malformed");
    if (!read_callback_(shared_from_this(), input_message_, error))
      Close();
    return;
  }
  coded_input_stream_->PopLimit(input_limit_);
  coded_input_stream_.reset();
  state_.store(IDLE);
  if (!read_callback_(shared_from_this(), input_message_, error))
    Close();
}

void Connection::CanSend() {
  Error error;
  assert(state_.load() == WAITING_SEND);

  coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));

  coded_output_stream_->WriteVarint32(output_message_.ByteSize());
  if (!output_message_.SerializeToCodedStream(coded_output_stream_.get())) {
    error.set_code(Error::NETWORK);
    error.set_description("Can't send whole message at once");
    if (!send_callback_(shared_from_this(), error))
      Close();
    return;
  }
  coded_output_stream_.reset();
  file_output_stream_.Flush();
  state_.store(IDLE);
  if (!send_callback_(shared_from_this(), error))
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
