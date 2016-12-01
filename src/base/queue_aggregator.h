#pragma once

#include <base/locked_queue.h>
#include <base/thread.h>

#include STL(condition_variable)

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator {
 public:
  using Optional = typename LockedQueue<T>::Optional;

  ~QueueAggregator() noexcept(false) { DCHECK(closed_); }

  void Close() {
    {
      UniqueLock lock(orders_mutex_);
      closed_ = true;
      order_count_ = 0;
      pop_condition_.notify_all();
      done_condition_.notify_all();
    }

    // All aggregated queues should be closed before aggregator do.
    // Also should be explicitly closed before destruction.
    for (const auto& queue : queues_) {
      // Evaluate to prevent "unused variable" warning.
      DCHECK_O_EVAL(queue->closed_);
    }

    for (auto& thread : threads_) {
      thread.join();
    }
    threads_.clear();
  }

  void Aggregate(LockedQueue<T>* WEAK_PTR queue) THREAD_UNSAFE {
    queues_.push_back(queue);
    threads_.emplace_back("Queue Aggregator"_l, &QueueAggregator<T>::DoPop,
                          this, queue);
  }

  Optional Pop() THREAD_SAFE {
    UniqueLock lock(orders_mutex_);
    if (!closed_) {
      ++order_count_;
    }
    pop_condition_.notify_all();

    done_condition_.wait(lock, [this] { return closed_ || !orders_.empty(); });

    if (closed_ && orders_.empty()) {
      lock.unlock();

      for (auto queue : queues_) {
        Optional&& obj = queue->Pop();
        if (obj) {
          return std::move(obj);
        }
      }

      return Optional();
    }

    Optional&& obj = std::move(orders_.front());
    orders_.pop_front();
    return std::move(obj);
  }

 private:
  void DoPop(LockedQueue<T>* WEAK_PTR queue) {
    while (!closed_) {
      {
        UniqueLock lock(orders_mutex_);
        pop_condition_.wait(lock, [this] { return order_count_ || closed_; });
      }

      UniqueLock lock(queue->pop_mutex_);
      queue->pop_condition_.wait(
          lock, [queue] { return queue->closed_ || !queue->queue_.empty(); });
      if (queue->closed_ && queue->queue_.empty()) {
        break;
      }

      {
        UniqueLock lock(orders_mutex_);

        if (order_count_) {
          orders_.push_back(std::move(queue->queue_.front()));
          queue->queue_.pop();
          --queue->size_;
          --order_count_;
          lock.unlock();
          done_condition_.notify_one();
        }
      }
    }
  }

  List<Thread> threads_;
  List<LockedQueue<T> * WEAK_PTR> queues_;

  std::mutex orders_mutex_;
  Atomic<bool> closed_ = {false};
  size_t order_count_ = 0;
  List<T> orders_;
  std::condition_variable pop_condition_;
  std::condition_variable done_condition_;
};

}  // namespace base
}  // namespace dist_clang
