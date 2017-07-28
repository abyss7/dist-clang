#pragma once

#include <base/locked_queue.h>

#include STL(condition_variable)
#include STL(deque)

namespace dist_clang {
namespace base {

template <class T>
class LockedQueue<T>::Index {
 public:
  using QueueIterator = typename LockedQueue<T>::Queue::iterator;
  using TasksList = List<QueueIterator>;
  struct Shard {
    TasksList tasks;
    std::condition_variable pop_condition_;
  };

  ~Index() {
    // Make sure we don't leak iterators.
    DCHECK(reverse_index_.size() == Size());
  }

  ui64 Size() const THREAD_UNSAFE {
    ui64 index_size = 0u;
    for (const auto& shard : index_) {
      index_size += shard.tasks.size();
    }
    return index_size;
  }

  void Put(QueueIterator it, const ui32 shard) {
    EnsureShardExists(shard);
    reverse_index_[&*it] = std::make_pair(
        index_[shard].tasks.insert(index_[shard].tasks.end(), it),
        shard);
  }

  bool ShardIsEmpty(const ui32 shard) THREAD_UNSAFE {
    DCHECK(shard < index_.size());
    return index_[shard].tasks.empty();
  }

  template <typename Pred>
  void WaitForShard(const ui32 shard, UniqueLock& lock,
                    const Seconds& timeout,
                    const Pred& pred) THREAD_UNSAFE {
    DCHECK(shard < index_.size());
    index_[shard].pop_condition_.wait_for(lock, timeout, pred);
  }

  void NotifyShard(const ui32 shard) THREAD_UNSAFE {
    DCHECK(shard < index_.size());
    index_[shard].pop_condition_.notify_one();
  }

  // Returns either |shard| or shard with queue larger than |shard_queue_limit|.
  ui32 MaybeOverloadedShard(const ui32 shard_queue_limit,
                            const ui32 shard) THREAD_UNSAFE {
    EnsureShardExists(shard);
    if (index_[shard].tasks.size()) {
      return shard;
    }

    for (ui32 current = 0; current < index_.size(); ++current) {
      if (index_[current].tasks.size() > shard_queue_limit) {
        return current;
      }
    }

    return shard;
  }

  QueueIterator GetWithHint(ui32 shard, QueueIterator begin) THREAD_UNSAFE {
    DCHECK(!index_.empty());

    QueueIterator item;
    if (shard < index_.size() && !index_[shard].tasks.empty()) {
      item = index_[shard].tasks.front();
      index_[shard].tasks.pop_front();
      reverse_index_.erase(&*item);
    } else {
      item = begin;
      shard = reverse_index_[&*begin].second;
      index_[shard].tasks.erase(reverse_index_[&*begin].first);
      reverse_index_.erase(&*begin);
    }

    return item;
  }

  QueueIterator GetStrict(const ui32 shard) THREAD_UNSAFE {
    DCHECK(shard < index_.size() && !index_[shard].tasks.empty());

    auto& tasks = index_[shard].tasks;
    QueueIterator item = tasks.front();
    tasks.pop_front();
    reverse_index_.erase(&*item);

    return item;
  }

 private:
  FRIEND_TEST(LockedQueueIndexTest, BasicUsage);
  FRIEND_TEST(LockedQueueIndexTest, GetWithHint);
  FRIEND_TEST(LockedQueueIndexTest, GetStrict);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrowsOnPut);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrowsOnOverloadedSearch);
  FRIEND_TEST(LockedQueueIndexTest, MaybeOverloadedReturnsOverloadedShard);

  void EnsureShardExists(const ui32 shard) {
    // TODO(ilezhankin): describe rationale for this place.
    if (shard >= index_.size()) {
      index_.resize(shard + 1);
    }
  }

  // Use deque as condition variables are not movable.
  std::deque<Shard> index_;

  // FIXME: use anything more shiny for hashing than |T*|.
  HashMap<const T*, Pair<typename TasksList::const_iterator, ui32 /* shard */>>
      reverse_index_;
  // This index is used to remove items from |index_| in O(1) when
  // those are picked from the top of the |queue_|.
};

}  // namespace base
}  // namespace dist_clang
