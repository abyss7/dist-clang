#include <net/connection.h>

#include <base/logging.h>

#include <poll.h>

#include <base/using_log.h>

namespace dist_clang {
namespace net {

template <>
bool Connection::SendAsync(ScopedMessage message, SendCallback callback) {
  message_ = std::move(message);
  return SendAsyncImpl(callback);
}

template <>
bool Connection::SendSync(ScopedMessage message, Status* status) {
  message_ = std::move(message);
  return SendSyncImpl(status);
}

bool Connection::IsPeerGone() const {
  if (IsClosed()) {
    return true;
  }

  const int fd = NativeHandle();
  if (fd == -1) {
    return false;
  }

  struct pollfd poll_fd = {fd, POLLRDHUP, 0};
  return poll(&poll_fd, 1, 0) == 1 &&
         (poll_fd.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

// static
Connection::SendCallback Connection::CloseAfterSend() {
  return [](ConnectionPtr, const Status& status) {
    if (status.code() != Status::OK) {
      LOG(ERROR) << "Failed to send message: " << status.description();
    }
    return false;
  };
}

bool Connection::ReportStatus(const Status& message, SendCallback callback) {
  message_.reset(new Message);
  message_->set_allocated_status(new Status(message));
  return SendAsyncImpl(callback);
}

}  // namespace net
}  // namespace dist_clang
