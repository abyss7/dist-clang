#pragma once

#include "base/attributes.h"

#include <atomic>
#include <condition_variable>
#include <experimental/optional>
#include <mutex>
#include <queue>

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator;

template <class T>
class LockedQueue {
  public:
    using Optional = std::experimental::optional<T>;

    enum { UNLIMITED = 0 };

    LockedQueue() = default;
    explicit LockedQueue(size_t capacity) : capacity_(capacity) {}

    // Should be explicitly closed before destruction.
    void Close() THREAD_SAFE {
      std::unique_lock<std::mutex> lock(pop_mutex_);
      closed_ = true;
      pop_condition_.notify_all();
    }

    inline size_t Size() const THREAD_SAFE {
      return size_;
    }

    // Returns |false| only when this queue is closed or when the capacity is
    // exceeded.
    bool Push(T obj) THREAD_SAFE {
      if (closed_) {
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(pop_mutex_);
        if (queue_.size() >= capacity_ && capacity_ != UNLIMITED) {
          return false;
        }
        queue_.push(std::move(obj));
      }
      ++size_;
      pop_condition_.notify_one();

      return true;
    }

    // Returns disengaged object only when this queue is closed and empty.
    Optional Pop() THREAD_SAFE {
      std::unique_lock<std::mutex> lock(pop_mutex_);
      pop_condition_.wait(lock, [this]{ return closed_ || !queue_.empty(); });
      if (closed_ && queue_.empty()) {
        return Optional();
      }
      Optional&& obj = std::move(queue_.front());
      queue_.pop();
      --size_;
      return std::move(obj);
    }

    Optional Pop(std::atomic<size_t> &external_counter) THREAD_SAFE {
      std::unique_lock<std::mutex> lock(pop_mutex_);
      pop_condition_.wait(lock, [this]{ return closed_ || !queue_.empty(); });
      if (closed_ && queue_.empty()) {
        return Optional();
      }
      Optional&& obj = std::move(queue_.front());
      queue_.pop();
      --size_;
      ++external_counter;
      return std::move(obj);
    }

  private:
    friend class QueueAggregator<T>;

    std::mutex pop_mutex_;
    std::condition_variable pop_condition_;
    std::queue<T> queue_;

    std::atomic<size_t> size_ = {0};
    const size_t capacity_ = UNLIMITED;
    std::atomic<bool> closed_ = {false};
};

}  // namespace base
}  // namespace dist_clang
