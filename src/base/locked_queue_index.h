#pragma once

#include <base/locked_queue.h>

namespace dist_clang {
namespace base {

template <class T>
class LockedQueue<T>::Index {
 public:
  using QueueIterator = typename LockedQueue<T>::Queue::iterator;
  using Shard = List<QueueIterator>;

  ~Index() {
    // Make sure we don't leak iterators.
    DCHECK(reverse_index_.size() == Size());
  }

  ui64 Size() const THREAD_UNSAFE {
    ui64 index_size = 0u;
    for (const auto& shard : index_) {
      index_size += shard.size();
    }
    return index_size;
  }

  void Put(QueueIterator it, ui32 shard) {
    // TODO(ilezhankin): describe rationale for this place.
    if (shard >= index_.size()) {
      index_.resize(shard + 1);
    }
    reverse_index_[&*it] =
        std::make_pair(index_[shard].insert(index_[shard].end(), it), shard);
  }

  QueueIterator Get(ui32 shard, QueueIterator begin) THREAD_UNSAFE {
    DCHECK(!index_.empty());

    QueueIterator item;
    if (shard < index_.size() && !index_[shard].empty()) {
      item = index_[shard].front();
      index_[shard].pop_front();
      reverse_index_.erase(&*item);
    } else {
      item = begin;
      shard = reverse_index_[&*begin].second;
      index_[shard].erase(reverse_index_[&*begin].first);
      reverse_index_.erase(&*begin);
    }

    return item;
  }

 private:
  FRIEND_TEST(LockedQueueIndexTest, BasicUsage);
  FRIEND_TEST(LockedQueueIndexTest, GetFromHead);
  FRIEND_TEST(LockedQueueIndexTest, ShardIndexGrows);
  Vector<Shard> index_;

  // FIXME: use anything more shiny for hashing than |T*|.
  HashMap<const T*, Pair<typename Shard::const_iterator, ui32 /* shard */>>
      reverse_index_;
  // This index is used to remove items from |index_| in O(1) when
  // those are picked from the top of the |queue_|.
};

}  // namespace base
}  // namespace dist_clang
