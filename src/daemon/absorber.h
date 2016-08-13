#pragma once

#include <base/locked_queue.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>

namespace dist_clang {
namespace daemon {

class Absorber : public CompilationDaemon {
 public:
  explicit Absorber(const proto::Configuration& configuration);
  virtual ~Absorber();

  bool Initialize() override;

 private:
  using Message = UniquePtr<proto::Remote>;
  using Task = Pair<net::ConnectionPtr, Message>;
  using Queue = base::LockedQueue<Task>;
  using Optional = Queue::Optional;

  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  cache::ExtraFiles GetExtraFiles(const proto::Remote* message);

  bool PrepareExtraFilesForCompiler(const cache::ExtraFiles& extra_files,
                                    const String& temp_dir_path,
                                    base::proto::Flags* flags,
                                    net::proto::Status* status);

  void DoExecute(const base::WorkerPool& pool);

  UniquePtr<Queue> tasks_;
  UniquePtr<base::WorkerPool> workers_;
};

}  // namespace daemon
}  // namespace dist_clang
