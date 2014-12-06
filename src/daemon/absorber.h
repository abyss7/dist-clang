#pragma once

#include <base/locked_queue.h>
#include <base/worker_pool.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class Absorber : public BaseDaemon {
 public:
  Absorber(const proto::Configuration& configuration);
  virtual ~Absorber();

  virtual bool Initialize() override;

 private:
  using Message = UniquePtr<proto::RemoteExecute>;
  using Task = Pair<net::ConnectionPtr, Message>;
  using Queue = base::LockedQueue<Task>;
  using Optional = Queue::Optional;

  virtual bool HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const proto::Status& status) override;

  void DoExecute(const Atomic<bool>& is_shutting_down);

  UniquePtr<Queue> tasks_;
  UniquePtr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
