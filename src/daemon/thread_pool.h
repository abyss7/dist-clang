#pragma once

#include "task_queue.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
  public:
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
