#pragma once

#include <base/assert.h>
#include <base/attributes.h>

#include STL(condition_variable)
#include STL(experimental/optional)

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

  LockedQueue() = default;
  explicit LockedQueue(ui32 capacity) : capacity_(capacity) {}
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
    return Pop(nullptr, shard);
  }

  Optional Pop(Atomic<ui64>* external_counter,
               ui32 shard = DEFAULT_SHARD) THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_ && queue_.empty()) {
      return Optional();
    }

    auto it = index_.Get(shard, queue_.begin());
    Optional&& obj = std::move(*it);
    queue_.erase(it);
    --size_;
    if (external_counter) {
      ++(*external_counter);
    }
    return std::move(obj);
  }

 private:
  friend class QueueAggregator<T>;

  class Index {
   public:
    using QueueIterator = typename Queue::iterator;
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
      CHECK(shard < index_.size());

      QueueIterator item;
      if (!index_[shard].empty()) {
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
    Vector<Shard> index_;

    // FIXME: use anything more shiny for hashing than |T*|.
    HashMap<const T*, Pair<typename Shard::const_iterator, ui32 /* shard */>>
        reverse_index_;
    // |reverse_index_| is used to remove items from |index_| in O(1) when
    // those are picked from the top of the |queue_|.
  };

  std::mutex pop_mutex_;
  std::condition_variable pop_condition_;
  Queue queue_;

  Atomic<ui64> size_ = {0};
  const ui64 capacity_ = UNLIMITED;
  Atomic<bool> closed_ = {false};
  Index index_;
};

}  // namespace base
}  // namespace dist_clang
