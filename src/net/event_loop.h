#pragma once

#include "base/attributes.h"
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

    bool Run() THREAD_SAFE;
    void Stop() THREAD_SAFE;

  protected:
    inline int GetConnectionDescriptor(const ConnectionPtr connection) const;
    inline void ConnectionDoRead(ConnectionPtr connection);
    inline void ConnectionDoSend(ConnectionPtr connection);
    inline bool ConnectionToggleWait(ConnectionPtr connection, bool new_wait);
    inline bool ConnectionAdd(ConnectionPtr connection);

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down) = 0;
    virtual void DoIOWork(const volatile bool& is_shutting_down) = 0;
    virtual void DoClosingWork(const volatile bool& is_shutting_down) = 0;

    std::atomic<bool> is_running_;
    std::thread listening_thread_, closing_thread_;
    std::vector<std::thread> io_threads_;
    volatile bool is_shutting_down_;
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

bool EventLoop::ConnectionToggleWait(ConnectionPtr connection, bool new_wait) {
  return connection->ToggleWait(new_wait);
}

bool EventLoop::ConnectionAdd(ConnectionPtr connection) {
  return connection->AddToEventLoop();
}

}  // namespace net
}  // namespace dist_clang
