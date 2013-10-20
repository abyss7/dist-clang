#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace dist_clang {
namespace daemon {

class ThreadPool {
  public:
    using Closure = std::function<void(void)>;
    enum { UNLIMITED = 0 };

    explicit ThreadPool(
        size_t capacity = UNLIMITED,
        size_t concurrency = std::thread::hardware_concurrency());
    ~ThreadPool();

    void Run();
    bool Push(const Closure& task);
    inline size_t QueueSize() const;

  private:
    void DoWork();

    const size_t capacity_;
    std::vector<std::thread> workers_;

    std::mutex tasks_mutex_;
    std::condition_variable tasks_condition_;
    bool is_shutting_down_;
    std::queue<Closure> tasks_;
    std::atomic<size_t> size_;
};

size_t ThreadPool::QueueSize() const {
  return size_.load();
}

}  // namespace daemon
}  // namespace dist_clang
