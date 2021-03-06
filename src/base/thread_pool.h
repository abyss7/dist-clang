#pragma once

#include <base/future.h>
#include <base/locked_queue.h>
#include <base/worker_pool.h>

namespace dist_clang {
namespace base {

class ThreadPool {
 public:
  using Closure = Fn<void(void)>;
  using Promise = Promise<bool>;
  using Task = Pair<Closure, Promise>;
  using TaskQueue = LockedQueue<Task>;
  using Optional = Promise::Optional;

  explicit ThreadPool(ui64 capacity = TaskQueue::UNLIMITED,
                      ui32 concurrency = std::thread::hardware_concurrency() *
                                         2);
  ~ThreadPool();

  void Run();
  Optional Push(const Closure& task);
  Optional Push(Closure&& task);

 private:
  void DoWork(const base::WorkerPool& pool);

  TaskQueue tasks_;
  WorkerPool pool_;
  ui32 concurrency_;
};

}  // namespace base
}  // namespace dist_clang
