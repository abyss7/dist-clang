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
  base::Assert(!IsListening(fd));
#endif
  base::Assert(!IsNonBlocking(fd));
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

bool Connection::SendAsync(const CustomMessage &message,
                           SendCallback callback) {
  message_.Clear();
  if (!ConvertCustomMessage(message, &message_, nullptr)) {
    return false;
  }
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

bool Connection::ReadSync(Message *message, Status *status) {
  if (!message) {
    if (status) {
      status->set_code(Status::EMPTY_MESSAGE);
      status->set_description("Message pointer is null");
    }
    return false;
  }

  unsigned size;
  GzipInputStream gzip_stream(&file_input_stream_, GzipInputStream::ZLIB);
  {
    CodedInputStream coded_stream(&gzip_stream);
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

  if (!message->ParseFromBoundedZeroCopyStream(&gzip_stream, size)) {
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

bool Connection::SendSync(const CustomMessage &message, Status *status) {
  if (&message != &message_) {
    message_.Clear();
    if (!ConvertCustomMessage(message, &message_, status))
      return false;
  }

  {
    GzipOutputStream::Options options;
    options.format = GzipOutputStream::ZLIB;
    GzipOutputStream gzip_stream(&file_output_stream_, options);
    CodedOutputStream coded_stream(&gzip_stream);
    coded_stream.WriteVarint32(message_.ByteSize());
    if (!message_.SerializeToCodedStream(&coded_stream)) {
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

void Connection::DoRead() {
  Status status;
  ReadSync(&message_, &status);
  base::Assert(!!read_callback_);
  auto read_callback = read_callback_;
  read_callback_ = BindedReadCallback();
  if (!read_callback(message_, status)) {
    Close();
  }
}

void Connection::DoSend() {
  Status status;
  SendSync(message_, &status);
  base::Assert(!!send_callback_);
  auto send_callback = send_callback_;
  send_callback_ = BindedSendCallback();
  if (!send_callback(status)) {
    Close();
  }
}

void Connection::Close() {
  bool old_closed = false;
  if (is_closed_.compare_exchange_strong(old_closed, true)) {
    event_loop_.RemoveConnection(fd_);
    read_callback_ = BindedReadCallback();
    send_callback_ = BindedSendCallback();
    // TODO: do the "polite" shutdown.
    close(fd_);
  }
}

bool Connection::ConvertCustomMessage(const CustomMessage &input,
                                      Message *output, Status *status) {
  auto input_descriptor = input.GetDescriptor();
  auto output_descriptor = Message::descriptor();

  if (input_descriptor == output_descriptor) {
    if (output) {
      output->CopyFrom(input);
    }
    return true;
  }

  const ::google::protobuf::FieldDescriptor* extension_field = nullptr;

  for (int i = 0; i < input_descriptor->extension_count(); ++i) {
    auto containing_type = input_descriptor->extension(i)->containing_type();
    if (containing_type == output_descriptor) {
      extension_field = input_descriptor->extension(i);
      break;
    }
  }
  if (!extension_field) {
    if (status) {
      status->set_code(Status::EMPTY_MESSAGE);
      status->set_description("Message of type " + input.GetTypeName() +
                              " can't be sent, since it doesn't extend " +
                              output_descriptor->full_name());
    }
    return false;
  }

  if (output) {
    auto reflection = output->GetReflection();
    auto output_field =
        reflection->FindKnownExtensionByName(extension_field->full_name());
    base::Assert(output_field);
    reflection->MutableMessage(output, output_field)->CopyFrom(input);
  }
  return true;
}

}  // namespace net
}  // namespace dist_clang
