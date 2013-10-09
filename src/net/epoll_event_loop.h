#pragma once

#include "base/attributes.h"
#include "base/read_write_lock.h"
#include "net/base/types.h"
#include "net/event_loop.h"

#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace dist_clang {
namespace net {

class EpollEventLoop: public EventLoop {
  public:
    using ConnectionCallback = std::function<void(fd_t, ConnectionPtr)>;

    EpollEventLoop(ConnectionCallback callback);
    ~EpollEventLoop();

    bool HandlePassive(fd_t fd) THREAD_UNSAFE;

    virtual bool ReadyForRead(ConnectionPtr connection) THREAD_SAFE override;
    virtual bool ReadyForSend(ConnectionPtr connection) THREAD_SAFE override;
    virtual void RemoveConnection(fd_t fd) override;

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down) override;
    virtual void DoIOWork(const volatile bool& is_shutting_down) override;

    bool ReadyForListen(fd_t fd);

    fd_t listen_fd_, io_fd_;
    ConnectionCallback callback_;

    // We need to store listening fds - to be able to close them at shutdown.
    std::unordered_set<fd_t> listening_fds_;

    base::ReadWriteMutex connections_mutex_;
    std::unordered_map<fd_t, net::ConnectionWeakPtr> connections_;
};

}  // namespace net
}  // namespace dist_clang
