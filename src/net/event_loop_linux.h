#pragma once

#include <base/file/epoll_linux.h>
#include <net/event_loop.h>
#include <net/passive.h>

namespace dist_clang {
namespace net {

class EpollEventLoop : public EventLoop {
 public:
  using ConnectionCallback = Fn<void(const Passive&, ConnectionPtr)>;

  EpollEventLoop(ConnectionCallback callback);
  ~EpollEventLoop();

  bool HandlePassive(Passive&& fd) THREAD_UNSAFE override;
  bool ReadyForRead(ConnectionImplPtr connection) THREAD_SAFE override;
  bool ReadyForSend(ConnectionImplPtr connection) THREAD_SAFE override;

 private:
  void DoListenWork(const base::WorkerPool& pool, base::Data& self) override;
  void DoIOWork(const base::WorkerPool& pool, base::Data& self) override;

  inline bool ReadyForListen(const Passive& fd) {
    return listen_.Update(fd, EPOLLIN | EPOLLONESHOT);
  }
  bool ReadyFor(ConnectionImplPtr connection, ui32 events);

  base::Epoll listen_, io_;
  ConnectionCallback callback_;

  // We need to store listening fds - to be able to close them at shutdown.
  HashSet<Passive> listening_fds_;
};

}  // namespace net
}  // namespace dist_clang
