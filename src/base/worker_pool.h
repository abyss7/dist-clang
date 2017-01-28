#pragma once

#include <base/aliases.h>
#include <base/file/pipe.h>

#include STL(thread)

namespace dist_clang {
namespace base {

class WorkerPool {
 public:
  using NetWorker = Fn<void(const WorkerPool&, Data&)>;
  using SimpleWorker = Fn<void(const WorkerPool&)>;

  explicit WorkerPool(bool force_shut_down = false);
  ~WorkerPool();

  void AddWorker(Literal name, const NetWorker& worker, ui32 count = 1);
  void AddWorker(Literal name, const SimpleWorker& worker, ui32 count = 1);

  bool WaitUntilShutdown(const Seconds& duration) const;

  bool IsShuttingDown() const { return WaitUntilShutdown(ZERO_DURATION); }

 private:
  Vector<Thread> workers_;
  Atomic<bool> is_shutting_down_, force_shut_down_;
  mutable std::mutex shutdown_mutex_;
  mutable std::condition_variable shutdown_condition_;
  Pipe self_;

  static const constexpr Seconds ZERO_DURATION = Seconds::zero();
};

}  // namespace base
}  // namespace dist_clang
