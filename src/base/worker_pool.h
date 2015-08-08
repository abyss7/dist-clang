#pragma once

#include <base/aliases.h>
#include <base/file/pipe.h>

#include STL(thread)

namespace dist_clang {
namespace base {

class WorkerPool {
 public:
  using NetWorker = Fn<void(const Atomic<bool>&, Data&)>;
  using SimpleWorker = Fn<void(const Atomic<bool>&)>;

  explicit WorkerPool(bool force_shut_down = false);
  ~WorkerPool();

  void AddWorker(Literal name, const NetWorker& worker, ui32 count = 1);
  void AddWorker(Literal name, const SimpleWorker& worker, ui32 count = 1);

 private:
  Vector<Thread> workers_;
  Atomic<bool> is_shutting_down_, force_shut_down_;
  Pipe self_;
};

}  // namespace base
}  // namespace dist_clang
