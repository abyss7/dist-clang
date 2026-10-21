#pragma once

#include <base/locked_queue.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>

namespace dist_clang {
namespace daemon {

class Absorber : public CompilationDaemon {
 public:
  explicit Absorber(const Configuration& conf);
  virtual ~Absorber();

  bool Initialize() override;

 private:
  using Message = UniquePtr<proto::Remote>;
  struct Task {
    net::ConnectionPtr connection;
    Message message;
    cache::string::HandledHash handled_hash;
  };
  using Queue = base::LockedQueue<Task>;
  using Optional = Queue::Optional;

  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  cache::ExtraFiles GetExtraFiles(const proto::Remote* message);

  bool PrepareExtraFilesForCompiler(const cache::ExtraFiles& extra_files,
                                    const Path& temp_dir,
                                    base::proto::Flags* flags,
                                    net::proto::Status* status);

  void DoCheckCache(const base::WorkerPool& pool);
  void DoExecute(const base::WorkerPool& pool);

  UniquePtr<Queue> tasks_, cache_tasks_;
  UniquePtr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
