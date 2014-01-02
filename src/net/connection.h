#pragma once

#include "net/connection_forward.h"

#include <functional>

namespace dist_clang {

namespace proto {
class Status;
class Universal;
}  // namespace proto

namespace net {

class Connection {
  public:
    template <typename S> using Fn = std::function<S>;
    template <class T> using UniquePtr = std::unique_ptr<T>;
    using Status = proto::Status;
    using Message = proto::Universal;
    using ScopedMessage = UniquePtr<Message>;
    using ReadCallback = Fn<bool(ConnectionPtr, ScopedMessage, const Status&)>;
    using SendCallback = Fn<bool(ConnectionPtr, const Status&)>;

    virtual ~Connection() {}

    virtual bool ReadAsync(ReadCallback callback) = 0;
    virtual bool ReadSync(Message* message, Status* status = nullptr) = 0;

  protected:
    ScopedMessage message_;

  private:
    virtual bool SendAsync(SendCallback callback) = 0;
    virtual bool SendSync(Status* status) = 0;
};

}  // namespace net
}  // namespace dist_clang
