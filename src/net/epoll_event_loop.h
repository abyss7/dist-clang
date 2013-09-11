#pragma once

#include "base/attributes.h"
#include "net/event_loop.h"

#include <functional>
#include <mutex>
#include <unordered_set>

namespace dist_clang {
namespace net {

class EpollEventLoop: public EventLoop {
  public:
    typedef std::function<void(int, ConnectionPtr)> ConnectionCallback;

    EpollEventLoop(ConnectionCallback callback);

    bool Handle(int fd) THREAD_SAFE;

    virtual bool ReadyForRead(ConnectionPtr connection) THREAD_SAFE override;
    virtual bool ReadyForSend(ConnectionPtr connection) THREAD_SAFE override;

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down) override;
    virtual void DoIncomingWork(const volatile bool& is_shutting_down) override;
    virtual void DoOutgoingWork(const volatile bool& is_shutting_down) override;

    bool ReadyForListen(int fd);
    ConnectionPtr AddConnection(int fd);

    int listen_fd_, incoming_fd_, outgoing_fd_;
    std::mutex listening_fds_mutex_, connections_mutex_;
    std::unordered_set<int> listening_fds_;
    std::unordered_set<ConnectionPtr> connections_;
    ConnectionCallback callback_;
};

}  // namespace net
}  // namespace dist_clang
