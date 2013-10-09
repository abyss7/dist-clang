#pragma once

#include "base/attributes.h"
#include "net/base/worker_pool.h"
#include "net/connection.h"

#include <thread>
#include <vector>

namespace dist_clang {
namespace net {

class EventLoop {
  public:
    explicit EventLoop(
        size_t concurrency = std::thread::hardware_concurrency());
    virtual ~EventLoop();

    virtual bool ReadyForRead(ConnectionPtr connection) = 0;
    virtual bool ReadyForSend(ConnectionPtr connection) = 0;
    virtual void RemoveConnection(fd_t fd) = 0;

    bool Run() THREAD_SAFE;
    void Stop() THREAD_SAFE;

  protected:
    inline int GetConnectionDescriptor(const ConnectionPtr connection) const;
    inline void ConnectionDoRead(ConnectionPtr connection);
    inline void ConnectionDoSend(ConnectionPtr connection);
    inline bool ConnectionAdd(ConnectionPtr connection);

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down) = 0;
    virtual void DoIOWork(const volatile bool& is_shutting_down) = 0;

    std::atomic<int> is_running_;
    size_t concurrency_;
    std::unique_ptr<WorkerPool> pool_;
};

int EventLoop::GetConnectionDescriptor(const ConnectionPtr connection) const {
  return connection->fd_;
}

void EventLoop::ConnectionDoRead(ConnectionPtr connection) {
  connection->DoRead();
}

void EventLoop::ConnectionDoSend(ConnectionPtr connection) {
  connection->DoSend();
}

bool EventLoop::ConnectionAdd(ConnectionPtr connection) {
  return connection->AddToEventLoop();
}

}  // namespace net
}  // namespace dist_clang
