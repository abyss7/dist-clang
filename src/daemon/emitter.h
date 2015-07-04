#pragma once

#include <base/queue_aggregator.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>

namespace dist_clang {
namespace daemon {

class Emitter : public CompilationDaemon {
 public:
  explicit Emitter(const proto::Configuration& configuration);
  virtual ~Emitter();

  bool Initialize() override;

 private:
  enum TaskIndex {
    CONNECTION = 0,
    MESSAGE = 1,
    SOURCE = 2,
  };

  using Message = UniquePtr<proto::LocalExecute>;
  using Task = Tuple<net::ConnectionPtr, Message, cache::string::HandledSource>;
  using Queue = base::LockedQueue<Task>;
  using QueueAggregator = base::QueueAggregator<Task>;
  using Optional = Queue::Optional;
  using ResolveFn = Fn<net::EndPointPtr()>;

  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const proto::Status& status) override;

  void DoCheckCache(const Atomic<bool>& is_shutting_down);
  void DoLocalExecute(const Atomic<bool>& is_shutting_down);
  void DoRemoteExecute(const Atomic<bool>& is_shutting_down,
                       ResolveFn resolver);

  UniquePtr<Queue> all_tasks_, cache_tasks_, failed_tasks_;
  UniquePtr<QueueAggregator> local_tasks_;
  UniquePtr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
