#pragma once

#include <net/event_loop.h>

namespace dist_clang {
namespace net {

class EpollEventLoop : public EventLoop {
 public:
  using ConnectionCallback = Fn<void(FileDescriptor, ConnectionPtr)>;

  EpollEventLoop(ConnectionCallback callback);
  ~EpollEventLoop();

  virtual bool HandlePassive(FileDescriptor fd) THREAD_UNSAFE override;
  virtual bool ReadyForRead(ConnectionImplPtr connection) THREAD_SAFE override;
  virtual bool ReadyForSend(ConnectionImplPtr connection) THREAD_SAFE override;

 private:
  virtual void DoListenWork(const std::atomic<bool>& is_shutting_down,
                            FileDescriptor self_pipe) override;
  virtual void DoIOWork(const std::atomic<bool>& is_shutting_down,
                        FileDescriptor self_pipe) override;

  bool ReadyForListen(FileDescriptor fd);
  bool ReadyFor(ConnectionImplPtr connection, ui32 events);

  FileDescriptor listen_fd_, io_fd_;
  ConnectionCallback callback_;

  // We need to store listening fds - to be able to close them at shutdown.
  HashSet<FileDescriptor> listening_fds_;
};

}  // namespace net
}  // namespace dist_clang
