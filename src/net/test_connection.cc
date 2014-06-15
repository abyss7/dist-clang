#include <net/test_connection.h>

namespace dist_clang {
namespace net {

TestConnection::TestConnection()
    : abort_on_send_(false),
      abort_on_read_(false),
      send_attempts_(nullptr),
      read_attempts_(nullptr),
      on_send_([](const Message &) {}),
      on_read_([](Message *) {}) {}

bool TestConnection::ReadAsync(ReadCallback callback) {
  if (read_attempts_) {
    (*read_attempts_)++;
  }

  if (abort_on_read_) {
    return false;
  }

  read_callback_ = callback;
  return true;
}

bool TestConnection::ReadSync(Message *message, Status *status) {
  if (read_attempts_) {
    (*read_attempts_)++;
  }

  if (abort_on_read_) {
    if (status) {
      status->set_description("Test connection aborts reading");
    }
    return false;
  }

  on_read_(message);
  return true;
}

void TestConnection::AbortOnSend() { abort_on_send_ = true; }

void TestConnection::AbortOnRead() { abort_on_read_ = true; }

void TestConnection::CountSendAttempts(ui32 *counter) {
  send_attempts_ = counter;
}

void TestConnection::CountReadAttempts(ui32 *counter) {
  read_attempts_ = counter;
}

void TestConnection::CallOnSend(Fn<void(const Message &)> callback) {
  on_send_ = callback;
}

void TestConnection::CallOnRead(Fn<void(Message *)> callback) {
  on_read_ = callback;
}

bool TestConnection::TriggerReadAsync(UniquePtr<proto::Universal> message,
                                      const proto::Status &status) {
  message->CheckInitialized();
  if (read_callback_) {
    return read_callback_(shared_from_this(), std::move(message), status);
  }

  return false;
}

bool TestConnection::SendAsyncImpl(SendCallback callback) {
  if (send_attempts_) {
    (*send_attempts_)++;
  }

  if (abort_on_send_) {
    return false;
  }

  on_send_(*message_.get());
  return true;
}

bool TestConnection::SendSyncImpl(Status *status) {
  if (send_attempts_) {
    (*send_attempts_)++;
  }

  if (abort_on_send_) {
    if (status) {
      status->set_description("Test connection aborts sending");
    }
    return false;
  }

  on_send_(*message_.get());
  return true;
}

}  // namespace net
}  // namespace dist_clang
