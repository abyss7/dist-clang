#pragma once

#include <base/attributes.h>
#include <base/worker_pool.h>
#include <net/connection_impl.h>

#include <thread>
#include <vector>

namespace dist_clang {
namespace net {

class EventLoop {
 public:
  explicit EventLoop(ui32 concurrency = std::thread::hardware_concurrency());
  virtual ~EventLoop();

  virtual bool HandlePassive(fd_t fd) = 0;
  virtual bool ReadyForRead(ConnectionImplPtr connection) = 0;
  virtual bool ReadyForSend(ConnectionImplPtr connection) = 0;

  bool Run() THREAD_SAFE;
  void Stop() THREAD_SAFE;

 protected:
  inline int GetConnectionDescriptor(ConnectionImplPtr connection);
  inline void ConnectionDoRead(ConnectionImplPtr connection);
  inline void ConnectionDoSend(ConnectionImplPtr connection);
  inline void ConnectionClose(ConnectionImplPtr connection);

 private:
  enum Status {
    IDLE,
    RUNNING,
    STOPPED,
  };

  virtual void DoListenWork(const std::atomic<bool>& is_shutting_down,
                            fd_t self_pipe) = 0;
  virtual void DoIOWork(const std::atomic<bool>& is_shutting_down,
                        fd_t self_pipe) = 0;

  std::atomic<Status> is_running_;
  ui32 concurrency_;
  UniquePtr<base::WorkerPool> pool_;
};

int EventLoop::GetConnectionDescriptor(ConnectionImplPtr connection) {
  return connection->fd_;
}

void EventLoop::ConnectionDoRead(ConnectionImplPtr connection) {
  connection->DoRead();
}

void EventLoop::ConnectionDoSend(ConnectionImplPtr connection) {
  connection->DoSend();
}

void EventLoop::ConnectionClose(ConnectionImplPtr connection) {
  connection->Close();
}

}  // namespace net
}  // namespace dist_clang
