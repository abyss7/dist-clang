#include "base/thread_pool.h"

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

bool ThreadPool::Push(const Closure& task) {
  return tasks_.Push(task);
}

bool ThreadPool::Push(Closure&& task) {
  return tasks_.Push(std::move(task));
}

void ThreadPool::DoWork(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    Optional task = std::move(tasks_.Pop());
    if (!task) {
      break;
    }
    (*task)();
  }
}

}  // namespace base
}  // namespace dist_clang
