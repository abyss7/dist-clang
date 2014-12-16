#pragma once

#include <net/event_loop.h>
#include <net/passive.h>

namespace dist_clang {
namespace net {

class KqueueEventLoop : public EventLoop {
 public:
  using ConnectionCallback = Fn<void(const Passive&, ConnectionPtr)>;

  KqueueEventLoop(ConnectionCallback callback);
  ~KqueueEventLoop();

  bool HandlePassive(Passive&& fd) THREAD_UNSAFE override;
  bool ReadyForRead(ConnectionImplPtr connection) THREAD_SAFE override;
  bool ReadyForSend(ConnectionImplPtr connection) THREAD_SAFE override;

 private:
  void DoListenWork(const Atomic<bool>& is_shutting_down,
                    base::Data& self) override;
  void DoIOWork(const Atomic<bool>& is_shutting_down,
                base::Data& self_pipe) override;

  bool ReadyForListen(const Passive& fd);
  bool ReadyFor(ConnectionImplPtr connection, i16 filter);

  // FIXME: implement |KQueue| based on |base::Handle|.
  base::Handle listen_, io_;
  ConnectionCallback callback_;

  // We need to store listening fds - to be able to close them at shutdown.
  HashSet<Passive> listening_fds_;
};

}  // namespace net
}  // namespace dist_clang
