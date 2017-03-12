#pragma once

#include <base/queue_aggregator.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>

namespace dist_clang {
namespace daemon {

class Emitter : public CompilationDaemon {
 public:
  explicit Emitter(const Configuration& conf);
  virtual ~Emitter();

  bool Initialize() override;

 protected:
  bool Check(const Configuration& conf) const override;
  bool Reload(const Configuration& conf) override;

 private:
  enum TaskIndex {
    CONNECTION = 0,
    MESSAGE = 1,
    SOURCE = 2,
    EXTRA_FILES = 3,
  };

  using Message = UniquePtr<base::proto::Local>;
  using Task = Tuple<net::ConnectionPtr, Message, cache::string::HandledSource,
                     cache::ExtraFiles>;
  using Queue = base::LockedQueue<Task>;
  using QueueAggregator = base::QueueAggregator<Task>;
  using Optional = Queue::Optional;
  using ResolveFn = Fn<net::EndPointPtr()>;

  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  void SetExtraFiles(const cache::ExtraFiles& extra_files,
                     proto::Remote* message);

  void SpawnRemoteWorkers();

  void DoCheckCache(const base::WorkerPool&);
  void DoLocalExecute(const base::WorkerPool&);
  void DoRemoteExecute(const base::WorkerPool&, ResolveFn resolver, ui32 shard);
  void DoPoll(const base::WorkerPool&, Vector<ResolveFn> resolvers);

  UniquePtr<Queue> all_tasks_, cache_tasks_, failed_tasks_;
  UniquePtr<QueueAggregator> local_tasks_;
  UniquePtr<base::WorkerPool> workers_;
  UniquePtr<base::WorkerPool> coordinator_workers_;
  UniquePtr<base::WorkerPool> remote_workers_;

  bool handle_all_tasks_ = true;
  // Indicates if we force shutdown of the remote workers pool: we shouldn't if
  // there is no coordinators, or if we stopped to poll coordinators.

  static const constexpr ui32 max_total_shards = 1024u;
};

}  // namespace daemon
}  // namespace dist_clang
