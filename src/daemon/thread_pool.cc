#include "daemon/thread_pool.h"

namespace dist_clang {
namespace daemon {

ThreadPool::ThreadPool(size_t capacity, size_t concurrency)
  : capacity_(capacity), workers_(concurrency), is_shutting_down_(false) {
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    is_shutting_down_ = true;
  }
  tasks_condition_.notify_all();

  for (auto& worker: workers_)
    worker.join();
}

void ThreadPool::Run() {
  for (std::thread& worker: workers_)
    worker = std::thread(&ThreadPool::DoWork, this);
}

bool ThreadPool::Push(const Closure& task) {
  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    if ((tasks_.size() >= capacity_ && capacity_ != UNLIMITED) ||
        is_shutting_down_)
      return false;
    tasks_.push(task);
  }
  tasks_condition_.notify_one();
  return true;
}

void ThreadPool::DoWork() {
  Closure task;

  do {
    {
      std::unique_lock<std::mutex> lock(tasks_mutex_);
      while(tasks_.empty() && !is_shutting_down_)
        tasks_condition_.wait(lock);
      if (is_shutting_down_ && tasks_.empty())
        break;
      task = tasks_.front();
      tasks_.pop();
    }
    task();
  } while (true);
}

}  // namespace daemon
}  // namespace dist_clang
