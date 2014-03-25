#pragma once

#include "base/future.h"
#include "base/locked_queue.h"
#include "base/worker_pool.h"
#include "net/base/types.h"

namespace dist_clang {
namespace base {

class ThreadPool {
  public:
    using Closure = std::function<void(void)>;
    using Promise = Promise<bool>;
    using Task = std::pair<Closure, Promise>;
    using TaskQueue = LockedQueue<Task>;
    using Optional = Promise::Optional;

    explicit ThreadPool(
        size_t capacity = TaskQueue::UNLIMITED,
        size_t concurrency = std::thread::hardware_concurrency());
    ~ThreadPool();

    void Run();
    Optional Push(const Closure& task);
    Optional Push(Closure&& task);
    inline size_t TaskCount() const;

  private:
    void DoWork(const std::atomic<bool>& is_shutting_down);

    TaskQueue tasks_;
    WorkerPool pool_;
    size_t concurrency_;
    std::atomic<size_t> active_task_count_ = {0};
};

size_t ThreadPool::TaskCount() const {
  return tasks_.Size() + active_task_count_;
}

}  // namespace base
}  // namespace dist_clang
