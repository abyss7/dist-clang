#pragma once

#include "net/connection_forward.h"

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

    bool Run();
    void Stop();

  protected:
    int GetConnectionDescriptor(const ConnectionPtr connection) const;
    void ConnectionCanRead(ConnectionPtr connection);
    void ConnectionCanSend(ConnectionPtr connection);

    volatile bool is_running_;

  private:
    virtual void DoListenWork(const volatile bool& is_shutting_down_) = 0;
    virtual void DoIncomingWork(const volatile bool& is_shutting_down) = 0;
    virtual void DoOutgoingWork(const volatile bool& is_shutting_down) = 0;
    virtual void DoClosingWork(const volatile bool& is_shutting_down) = 0;

    std::thread listening_thread_, closing_thread_;
    std::vector<std::thread> incoming_threads_, outgoing_threads_;
    volatile bool is_shutting_down_;
};

}  // namespace net
}  // namespace dist_clang
