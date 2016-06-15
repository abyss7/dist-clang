#pragma once

#include <base/attributes.h>
#include <base/worker_pool.h>
#include <net/connection_forward.h>

#include STL(thread)

namespace dist_clang {

namespace base {
class Data;
}

namespace net {

class Passive;

class EventLoop {
 public:
  explicit EventLoop(ui32 concurrency = std::thread::hardware_concurrency() *
                                        2);
  virtual ~EventLoop();

  virtual bool HandlePassive(Passive&& fd) = 0;
  virtual bool ReadyForRead(ConnectionImplPtr connection) = 0;
  virtual bool ReadyForSend(ConnectionImplPtr connection) = 0;

  bool Run() THREAD_SAFE;
  void Stop() THREAD_SAFE;

 protected:
  void ConnectionDoRead(ConnectionImplPtr connection);
  void ConnectionDoSend(ConnectionImplPtr connection);
  void ConnectionClose(ConnectionImplPtr connection);

 private:
  enum Status {
    IDLE,
    RUNNING,
    STOPPED,
  };

  virtual void DoListenWork(const base::WorkerPool& pool, base::Data& self) = 0;
  virtual void DoIOWork(const base::WorkerPool& pool, base::Data& self) = 0;

  Atomic<Status> is_running_;
  ui32 concurrency_;
  UniquePtr<base::WorkerPool> pool_;
};

}  // namespace net
}  // namespace dist_clang
