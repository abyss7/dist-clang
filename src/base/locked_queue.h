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
    explicit LockedQueue(size_t capacity);

    // Should be explicitly closed before destruction.
    void Close() THREAD_SAFE;

    inline size_t Size() const THREAD_SAFE;

    // Returns |false| only when this queue is closed or when the capacity is
    // exceeded.
    bool Push(T obj) THREAD_SAFE;

    // Returns disengaged object only when this queue is closed and empty.
    Optional Pop() THREAD_SAFE;

  private:
    friend class QueueAggregator<T>;

    std::mutex pop_mutex_;
    std::condition_variable pop_condition_;
    std::queue<T> queue_;

    std::atomic<size_t> size_ = {0};
    const size_t capacity_ = UNLIMITED;
    std::atomic<bool> closed_ = {false};
};

template <class T>
size_t LockedQueue<T>::Size() const {
  return size_;
}

}  // namespace base
}  // namespace dist_clang
