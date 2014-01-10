#pragma once

#include "net/connection.h"

namespace dist_clang {
namespace net {

class TestConnection: public Connection {
  public:
    TestConnection();

    virtual bool ReadAsync(ReadCallback callback) override;
    virtual bool ReadSync(Message *message, Status *status) override;

    void AbortOnSend();
    void AbortOnRead();
    void CountSendAttempts(uint* counter);
    void CountReadAttempts(uint* counter);
    void CallOnSend(std::function<void(const Message&)> callback);
    void CallOnRead(std::function<void(Message*)> callback);

  private:
    virtual bool SendAsyncImpl(SendCallback callback) override;
    virtual bool SendSyncImpl(Status *status) override;

    bool abort_on_send_, abort_on_read_;
    uint* send_attempts_;
    uint* read_attempts_;
    std::function<void(const Message&)> on_send_;
    std::function<void(Message*)> on_read_;
};

}  // namespace net
}  // namespace dist_clang
