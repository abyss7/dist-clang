#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace dist_clang {
namespace daemon {

class ThreadPool {
  public:
    typedef std::function<void(void)> Closure;

    explicit ThreadPool(
        size_t capacity,
        size_t concurrency = std::thread::hardware_concurrency());
    ~ThreadPool();

    void Run();
    bool Push(const Closure& task);

  private:
    void DoWork();

    const size_t capacity_;
    std::vector<std::thread> workers_;

    std::mutex tasks_mutex_;
    std::condition_variable tasks_condition_;
    bool is_shutting_down_;
    std::queue<Closure> tasks_;
};

}  // namespace daemon
}  // namespace dist_clang
