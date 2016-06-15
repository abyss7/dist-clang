#pragma once

#include <base/aliases.h>
#include <base/file/pipe.h>

#include STL(thread)

namespace dist_clang {
namespace base {

class WorkerPool {
 public:
  using NetWorker = Fn<void(WorkerPool&, Data&)>;
  using SimpleWorker = Fn<void(WorkerPool&)>;

  explicit WorkerPool(bool force_shut_down = false);
  ~WorkerPool();

  void AddWorker(Literal name, const NetWorker& worker, ui32 count = 1);
  void AddWorker(Literal name, const SimpleWorker& worker, ui32 count = 1);

  bool WaitUntilShutdown(const std::chrono::seconds& duration);

  bool IsShuttingDown() {
    return WaitUntilShutdown(ZERO_DURATION);
  }

 private:
  Vector<Thread> workers_;
  Atomic<bool> is_shutting_down_, force_shut_down_;
  std::mutex shutdown_mutex_;
  std::condition_variable shutdown_condition_;
  Pipe self_;

  static const std::chrono::seconds ZERO_DURATION;
};

}  // namespace base
}  // namespace dist_clang
