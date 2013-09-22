#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace dist_clang {
namespace daemon {

// Thit thread pool has two queues. The one is unlimited and has high priority,
// the other one has limited size and has low priority.
class ThreadPool {
  public:
    typedef std::function<void(void)> Closure;
    enum { UNLIMITED = 0 };

    explicit ThreadPool(
        size_t capacity = UNLIMITED,
        size_t concurrency = std::thread::hardware_concurrency());
    ~ThreadPool();

    void Run();
    bool Push(const Closure& task);
    void PushInternal(const Closure& task);
    inline size_t InternalCount() const;

  private:
    void DoWork();

    // |capacity_| limits size of a public queue.
    // An internal queue is unlimited.
    const size_t capacity_;
    std::vector<std::thread> workers_;

    std::mutex tasks_mutex_;
    std::condition_variable tasks_condition_;
    bool is_shutting_down_;
    std::queue<Closure> public_tasks_, internal_tasks_;
};

size_t ThreadPool::InternalCount() const {
  return internal_tasks_.size();
}

}  // namespace daemon
}  // namespace dist_clang
