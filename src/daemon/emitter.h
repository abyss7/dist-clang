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
  FRIEND_TEST(EmitterTest, GracefulConfigurationUpdate);
  friend class CoordinatedTestEmitter;
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

  virtual void UpdateCompilationPool(const proto::Configuration& configuration);
  void SpawnCompilationWorker(const proto::Host& remote);

  void DoCheckCache(const base::WorkerPool&);
  void DoLocalExecute(const base::WorkerPool&);
  void DoRemoteExecute(const base::WorkerPool&, ResolveFn resolver);
  void DoCoordinator(const base::WorkerPool&, std::vector<ResolveFn> resolvers);

  UniquePtr<Queue> all_tasks_, cache_tasks_, failed_tasks_;
  UniquePtr<QueueAggregator> local_tasks_;
  UniquePtr<base::WorkerPool> workers_;
  UniquePtr<base::WorkerPool> compilation_workers_;
  Vector<proto::Host> remotes_;

  bool runs_coordinators_task_;
  std::chrono::minutes coordinator_poll_time_;
};

}  // namespace daemon
}  // namespace dist_clang
