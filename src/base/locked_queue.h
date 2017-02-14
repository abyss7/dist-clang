#pragma once

#include <base/assert.h>
#include <base/attributes.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

#include STL(condition_variable)
#include STL_EXPERIMENTAL(optional)

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator;

// If the queue is shardless then by default it uses only zero shard.
template <class T>
class LockedQueue {
 public:
  using Optional = std::experimental::optional<T>;
  using Queue = List<T>;

  enum {
    UNLIMITED = 0,
    DEFAULT_SHARD = 0,
  };

  explicit LockedQueue(ui32 capacity = UNLIMITED)
      : capacity_(capacity), timeout_(Seconds::zero()) {}
  explicit LockedQueue(Seconds pop_timeout) : timeout_(pop_timeout) {
    DCHECK(timeout_ > Seconds::zero());
  }
  ~LockedQueue() {
    DCHECK(closed_);
    DCHECK(index_.Size() == queue_.size());
  }

  // Should be explicitly closed before destruction.
  void Close() THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    closed_ = true;
    pop_condition_.notify_all();
  }

  inline ui32 Size() const THREAD_SAFE { return size_; }

  // Returns |false| only when this queue is closed or when the capacity is
  // exceeded.
  bool Push(T obj, ui32 shard = DEFAULT_SHARD) THREAD_SAFE {
    if (closed_) {
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(pop_mutex_);
      if (queue_.size() >= capacity_ && capacity_ != UNLIMITED) {
        return false;
      }
      index_.Put(queue_.insert(queue_.end(), std::move(obj)), shard);
    }
    ++size_;
    pop_condition_.notify_one();

    return true;
  }

  // Returns disengaged object only when this queue is closed and empty.
  Optional Pop(ui32 shard = DEFAULT_SHARD) THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_ && queue_.empty()) {
      return Optional();
    }

    auto it = index_.Get(shard, queue_.begin());
    Optional&& obj = std::move(*it);
    queue_.erase(it);
    --size_;
    return std::move(obj);
  }

 private:
  FRIEND_TEST(LockedQueueIndexTest, BasicUsage);
  FRIEND_TEST(LockedQueueIndexTest, GetFromHead);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrows);
  friend class QueueAggregator<T>;

  class Index;

  std::mutex pop_mutex_;
  std::condition_variable pop_condition_;
  Queue queue_;
  Index index_;

  const ui64 capacity_ = UNLIMITED;
  const Seconds timeout_ = Seconds::zero();

  Atomic<ui64> size_ = {0};
  Atomic<bool> closed_ = {false};
};

}  // namespace base
}  // namespace dist_clang

#include <base/locked_queue_index.h>
