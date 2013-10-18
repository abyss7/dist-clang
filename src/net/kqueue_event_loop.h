#pragma once

#include "base/attributes.h"
#include "net/event_loop.h"

namespace dist_clang {
namespace net {

class KqueueEventLoop: public EventLoop {
  public:
    using ConnectionCallback = std::function<void(fd_t, ConnectionPtr)>;

    KqueueEventLoop(ConnectionCallback callback);
    ~KqueueEventLoop();

    virtual bool HandlePassive(fd_t fd) THREAD_UNSAFE override;
    virtual bool ReadyForRead(ConnectionPtr connection) THREAD_SAFE override;
    virtual bool ReadyForSend(ConnectionPtr connection) THREAD_SAFE override;
    virtual void RemoveConnection(fd_t fd) THREAD_SAFE override;

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down,
                              fd_t self_pipe) override;
    virtual void DoIOWork(const volatile bool& is_shutting_down,
                          fd_t self_pipe) override;

    fd_t listen_fd_, io_fd_;
    ConnectionCallback callback_;
};

}  // namespace net
}  // namespace dist_clang
