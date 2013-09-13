#include "net/connection.h"

#include "net/base/utils.h"
#include "net/event_loop.h"

#include <cstdint>

namespace dist_clang {
namespace net {

// static
ConnectionPtr Connection::Create(EventLoop &event_loop, fd_t fd) {
  if (!MakeNonBlocking(fd))
    return ConnectionPtr();
  return ConnectionPtr(new Connection(event_loop, fd));
}

Connection::Connection(EventLoop& event_loop, fd_t fd)
  : fd_(fd), event_loop_(event_loop), is_closed_(false), state_(IDLE),
    file_input_stream_(fd_), input_limit_(0), file_output_stream_(fd_) {}

Connection::~Connection() {
  Close();
}

bool Connection::Read(ReadCallback callback) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_READ))
    return false;

  read_callback_ = callback;
  event_loop_.ReadyForRead(shared_from_this());
  return true;
}

bool Connection::Send(const Message &message, SendCallback callback) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, WAITING_SEND))
    return false;

  send_callback_ = callback;
  output_message_.CopyFrom(message);
  event_loop_.ReadyForSend(shared_from_this());
  return true;
}

bool Connection::Read(Message *message, Error *error) {
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

bool Connection::Send(const Message &message, Error *error) {
  State old_state = IDLE;
  if (!state_.compare_exchange_strong(old_state, SENDING)) {
    if (error) {
      error->set_code(Error::INCONSEQUENT);
      error->set_description("Can't send synchronously while other operation "
                             "is in progress");
    }
    return false;
  }

  if (!MakeNonBlocking(fd_, true)) {
    if (error) {
      error->set_code(Error::NETWORK);
      error->set_description("Can't make socket a blocking one");
    }
    return false;
  }

  coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));

  coded_output_stream_->WriteVarint32(message.ByteSize());
  auto sent_bytes = coded_output_stream_->ByteCount();
  if (!message.SerializeToCodedStream(coded_output_stream_.get()) ||
      coded_output_stream_->ByteCount() != sent_bytes + message.ByteSize()) {
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
  assert(state_.load() == WAITING_READ || state_.load() == READING);

  if (state_.load() == WAITING_READ) {
    input_message_.Clear();
    unsigned message_size;

    coded_input_stream_.reset(new CodedInputStream(&file_input_stream_));

    if (!coded_input_stream_->ReadVarint32(&message_size)) {
      Close();
      Error error;
      error.set_code(Error::NETWORK);
      error.set_description("Can't read incoming message size");
      read_callback_(ConnectionPtr(), input_message_, error);
      return;
    }
    input_limit_ = coded_input_stream_->PushLimit(message_size);
    state_.store(READING);
  }

  input_message_.MergePartialFromCodedStream(coded_input_stream_.get());
  if (coded_input_stream_->BytesUntilLimit() > 0)
    event_loop_.ReadyForRead(shared_from_this());
  else if (!input_message_.IsInitialized() ||
           !coded_input_stream_->ConsumedEntireMessage()) {
    Close();
    Error error;
    error.set_code(Error::BAD_MESSAGE);
    error.set_description("Incoming message is malformed");
    read_callback_(ConnectionPtr(), input_message_, error);
    return;
  }
  coded_input_stream_->PopLimit(input_limit_);
  coded_input_stream_.reset();
  state_.store(IDLE);
  read_callback_(shared_from_this(), input_message_, Error());
}

void Connection::CanSend() {
  assert(state_.load() == WAITING_SEND);

  coded_output_stream_.reset(new CodedOutputStream(&file_output_stream_));

  coded_output_stream_->WriteVarint32(output_message_.ByteSize());
  if (!output_message_.SerializeToCodedStream(coded_output_stream_.get())) {
    Close();
    Error error;
    error.set_code(Error::NETWORK);
    error.set_description("Can't send whole message at once");
    send_callback_(ConnectionPtr(), error);
    return;
  }
  coded_output_stream_.reset();
  state_.store(IDLE);
  send_callback_(shared_from_this(), Error());
}

void Connection::Close() {
  if (!is_closed_) {
    is_closed_ = true;
    state_.store(IDLE);
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
