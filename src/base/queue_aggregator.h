#pragma once

#include "base/locked_queue.h"

#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator {
  public:
    QueueAggregator();
    void Close();

    void Aggregate(LockedQueue<T>* WEAK_PTR queue) THREAD_UNSAFE;
    bool Pop(T& obj) THREAD_SAFE;

  private:
    void DoPop(LockedQueue<T>* WEAK_PTR queue);

    std::atomic<bool> closed_;
    std::list<std::thread> threads_;
    std::list<LockedQueue<T>* WEAK_PTR> queues_;

    std::mutex orders_mutex_;
    size_t order_count_;
    std::list<T> orders_;
    std::condition_variable pop_condition_;
    std::condition_variable done_condition_;
};

}  // namespace base
}  // namespace dist_clang
