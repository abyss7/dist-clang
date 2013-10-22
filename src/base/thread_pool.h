#pragma once

#include "base/locked_queue.h"
#include "base/worker_pool.h"
#include "net/base/types.h"

#include <thread>

namespace dist_clang {
namespace base {

class ThreadPool {
  public:
    using Closure = std::function<void(void)>;
    using LockedQueue = LockedQueue<Closure>;

    explicit ThreadPool(
        size_t capacity = LockedQueue::UNLIMITED,
        size_t concurrency = std::thread::hardware_concurrency());
    ~ThreadPool();

    void Run();
    bool Push(const Closure& task);
    inline size_t QueueSize() const;

  private:
    void DoWork(const volatile bool& is_shutting_down, net::fd_t self_pipe);

    LockedQueue tasks_;
    WorkerPool pool_;
    size_t concurrency_;
};

size_t ThreadPool::QueueSize() const {
  return tasks_.Size();
}

}  // namespace base
}  // namespace dist_clang
