#pragma once

#include "task_queue.h"

#include <cstddef>  // for |size_t|
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

    std::vector<std::thread> workers_;
    TaskQueue free_nodes_, busy_nodes_;
    bool is_empty_, is_shutting_down_;
};
