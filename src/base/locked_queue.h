#pragma once

#include <base/assert.h>
#include <base/attributes.h>
#include <base/worker_pool.h>

#include <third_party/gtest/exported/include/gtest/gtest_prod.h>

#include STL(condition_variable)
#include STL(limits)
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

  enum : ui32 {
    UNLIMITED = 0u,
    DEFAULT_SHARD = 0u,
    NOT_STRICT_SHARDING = 0u,
    GET_SHARD_DOWN = std::numeric_limits<ui32>::max(),
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
    index_.NotifyAllShards();
    aggregator_condition_.notify_all();
  }

  inline ui32 Size() const THREAD_SAFE { return size_; }

  bool IsClosed() const THREAD_SAFE { return closed_; }

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
    index_.NotifyShard(shard);

    // See a comment on |aggregator_condition_| to see why two notifications
    // should be done here.
    aggregator_condition_.notify_one();

    return true;
  }

  // Returns disengaged object only when this queue is closed and empty.
  Optional Pop() THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    index_.WaitDefaultShard(lock, [this] {
      return closed_ || !queue_.empty();
    });
    if (closed_ && queue_.empty()) {
      return Optional();
    }

    return RemoveTaskFromQueue(
        index_.GetWithHint(DEFAULT_SHARD, queue_.begin()));
  }

  // Returns disengaged object only when this queue is closed and empty, or pool
  // is shutting down.
  Optional Pop(const WorkerPool& pool,
               const ui32 shard_queue_limit = NOT_STRICT_SHARDING,
               const ui32 shard = DEFAULT_SHARD) THREAD_SAFE {
    DCHECK(timeout_ > Seconds::zero());

    UniqueLock lock(pop_mutex_);
    if (shard_queue_limit == NOT_STRICT_SHARDING) {
      return PopWithHint(pool, lock, shard);
    } else {
      return PopStrict(pool, lock, shard_queue_limit, shard);
    }
  }

 private:
  Optional PopWithHint(const WorkerPool& pool,
                       UniqueLock& lock,
                       const ui32 shard) THREAD_UNSAFE {
    // One can't wait for condition using predicate and timed wait here as once
    // the waiting timed out, the |pool.IsShuttingDown()| should be checked and
    // wait again if pool doesn't shutting down.
    while (!closed_ && queue_.empty() && !pool.IsShuttingDown()) {
      index_.WaitShardFor(shard, lock, timeout_);
    }
    if ((closed_ && queue_.empty()) || pool.IsShuttingDown()) {
      return Optional();
    }
    return RemoveTaskFromQueue(index_.GetWithHint(shard, queue_.begin()));
  }

  Optional PopStrict(const WorkerPool& pool,
                     UniqueLock& lock,
                     const ui32 shard_queue_limit,
                     const ui32 shard) THREAD_UNSAFE {
    DCHECK(shard_queue_limit != NOT_STRICT_SHARDING);
    // Check if there's a shard with number of tasks that exceed queue limit
    // AND current shard has no tasks to do. If so, steal a task from overloaded
    // shard.
    const ui32 maybe_overloaded_shard =
        index_.MaybeOverloadedShard(shard_queue_limit, shard);
    if (shard != maybe_overloaded_shard) {
      return RemoveTaskFromQueue(index_.GetStrict(maybe_overloaded_shard));
    }

    auto shard_is_closed = [&]{
      return closed_ || shard_queue_limit == GET_SHARD_DOWN;
    };

    // See comment about while loop in |PopWithHint|.
    while (!shard_is_closed()
           && index_.ShardIsEmpty(shard)
           && !pool.IsShuttingDown()) {
      index_.WaitShardFor(shard, lock, timeout_);
    }

    if ((shard_is_closed() && index_.ShardIsEmpty(shard))
        || pool.IsShuttingDown()) {
      return Optional();
    }
    return RemoveTaskFromQueue(index_.GetStrict(shard));
  }

  Optional RemoveTaskFromQueue(
        typename Queue::iterator task_iterator) THREAD_UNSAFE {
    Optional&& task = std::move(*task_iterator);
    queue_.erase(task_iterator);
    --size_;
    return std::move(task);
  }

  FRIEND_TEST(LockedQueueIndexTest, BasicUsage);
  FRIEND_TEST(LockedQueueIndexTest, GetWithHintFromHead);
  FRIEND_TEST(LockedQueueIndexTest, GetStrict);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrowsOnPut);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrowsOnOverloadedSearch);
  FRIEND_TEST(LockedQueueIndexTest, MaybeOverloadedReturnsOverloadedShard);
  friend class QueueAggregator<T>;

  class Index;

  std::mutex pop_mutex_;

  // Use a separate condition to allow QueueAggregator to know when new tasks
  // were pushed to queue to pick one, as it shouldn't wait on specific shard.
  std::condition_variable aggregator_condition_;

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
