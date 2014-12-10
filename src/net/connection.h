#pragma once

#include <net/connection_forward.h>
#include <proto/remote.pb.h>

namespace dist_clang {
namespace net {

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using Status = proto::Status;
  using Message = proto::Universal;
  using ScopedMessage = UniquePtr<Message>;
  using ReadCallback = Fn<bool(ConnectionPtr, ScopedMessage, const Status&)>;
  using SendCallback = Fn<bool(ConnectionPtr, const Status&)>;

  virtual ~Connection() {}

  virtual bool IsClosed() const = 0;

  virtual bool ReadAsync(ReadCallback callback) = 0;
  virtual bool ReadSync(Message* message, Status* status = nullptr) = 0;

  template <class M>
  bool SendAsync(UniquePtr<M> message,
                 SendCallback callback = CloseAfterSend()) {
    message_.reset(new Message);
    message_->SetAllocatedExtension(M::extension, message.release());
    return SendAsyncImpl(callback);
  }

  template <class M>
  bool SendSync(UniquePtr<M> message, Status* status = nullptr) {
    message_.reset(new Message);
    message_->SetAllocatedExtension(M::extension, message.release());
    return SendSyncImpl(status);
  }

  static SendCallback CloseAfterSend();

  bool ReportStatus(const Status& message,
                    SendCallback callback = CloseAfterSend());

 protected:
  ScopedMessage message_;

 private:
  virtual bool SendAsyncImpl(SendCallback callback) = 0;
  virtual bool SendSyncImpl(Status* status) = 0;
};

template <>
bool Connection::SendAsync(ScopedMessage message, SendCallback callback);

template <>
bool Connection::SendSync(ScopedMessage message, Status* status);

}  // namespace net
}  // namespace dist_clang
