#include "base/thread_pool.h"

#include "base/future_impl.h"
#include "base/locked_queue_impl.h"

using namespace std::placeholders;

namespace dist_clang {
namespace base {

ThreadPool::ThreadPool(size_t capacity, size_t concurrency)
  : tasks_(capacity), concurrency_(concurrency) {
}

ThreadPool::~ThreadPool() {
  tasks_.Close();
}

void ThreadPool::Run() {
  WorkerPool::SimpleWorker worker = std::bind(&ThreadPool::DoWork, this, _1);
  pool_.AddWorker(worker, concurrency_);
}

ThreadPool::Optional ThreadPool::Push(const Closure& task) {
  Promise promise(false);
  auto future = promise.GetFuture();
  if (tasks_.Push({task, std::move(promise)})) {
    return future;
  }

  return Optional();
}

ThreadPool::Optional ThreadPool::Push(Closure&& task) {
  Promise promise(false);
  auto future = promise.GetFuture();
  if (tasks_.Push({std::move(task), std::move(promise)})) {
    return future;
  }

  return Optional();
}

void ThreadPool::DoWork(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    TaskQueue::Optional&& task = tasks_.Pop();
    if (!task) {
      break;
    }
    task->first();
    task->second.SetValue(true);
  }
}

}  // namespace base
}  // namespace dist_clang
