#pragma once

#include <base/assert.h>
#include <base/attributes.h>

#include <third_party/libcxx/exported/include/atomic>
#include <third_party/libcxx/exported/include/condition_variable>
#include <third_party/libcxx/exported/include/experimental/optional>
#include <third_party/libcxx/exported/include/mutex>
#include <third_party/libcxx/exported/include/queue>

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator;

template <class T>
class LockedQueue {
 public:
  using Optional = std::experimental::optional<T>;

  enum {
    UNLIMITED = 0,
  };

  LockedQueue() = default;
  explicit LockedQueue(ui32 capacity) : capacity_(capacity) {}
  ~LockedQueue() { DCHECK(closed_); }

  // Should be explicitly closed before destruction.
  void Close() THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    closed_ = true;
    pop_condition_.notify_all();
  }

  inline ui32 Size() const THREAD_SAFE { return size_; }

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
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_ && queue_.empty()) {
      return Optional();
    }
    Optional&& obj = std::move(queue_.front());
    queue_.pop();
    --size_;
    return std::move(obj);
  }

  Optional Pop(std::atomic<ui64>& external_counter) THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
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

  std::atomic<ui64> size_ = {0};
  const ui64 capacity_ = UNLIMITED;
  std::atomic<bool> closed_ = {false};
};

}  // namespace base
}  // namespace dist_clang
