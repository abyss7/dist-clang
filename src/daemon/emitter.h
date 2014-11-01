#pragma once

#include <base/queue_aggregator.h>
#include <daemon/base_daemon.h>

namespace dist_clang {
namespace daemon {

class Emitter : public BaseDaemon {
 public:
  Emitter(const proto::Configuration& configuration);
  virtual ~Emitter();

  virtual bool Initialize() override;

 private:
  enum TaskIndex {
    CONNECTION = 0,
    MESSAGE = 1,
    SOURCE = 2,
  };

  using Message = UniquePtr<proto::LocalExecute>;
  using Task =
      Tuple<net::ConnectionPtr, Message, file_cache::string::HandledSource>;
  using Queue = base::LockedQueue<Task>;
  using QueueAggregator = base::QueueAggregator<Task>;
  using Optional = Queue::Optional;

  virtual bool HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const proto::Status& status) override;

  void DoCheckCache(const std::atomic<bool>& is_shutting_down);
  void DoLocalExecute(const std::atomic<bool>& is_shutting_down);
  void DoRemoteExecute(const std::atomic<bool>& is_shutting_down,
                       net::EndPointResolver::Optional end_point);

  UniquePtr<Queue> all_tasks_, cache_tasks_, failed_tasks_;
  UniquePtr<QueueAggregator> local_tasks_;
  UniquePtr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
