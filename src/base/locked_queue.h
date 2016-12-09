#pragma once

#include <base/assert.h>
#include <base/attributes.h>

#include STL(condition_variable)
#include STL(experimental/optional)
#include STL(list)

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator;

template <class T>
class LockedQueue {
 public:
  using Optional = std::experimental::optional<T>;
  using Queue = List<T>;
  enum InfoIndex {
    ITERATOR = 0u,
    HASH = 1u,
  };
  enum ReverseInfoIndex {
    RANGE = 0u,
    REVERSE_ITERATOR = 1u,
  };
  using TaskInfo = Tuple<typename Queue::iterator, size_t>;
  // List of tasks from same 'range' in terms of distributed tasks.
  using RangeList = List<TaskInfo>;

  enum {
    UNLIMITED = 0,
  };

  LockedQueue() = default;
  explicit LockedQueue(ui32 capacity) : capacity_(capacity) {}
  ~LockedQueue() {
    DCHECK(closed_);
#if !defined(NDEBUG)
    // Make sure we don't leak tasks.
    DCHECK(reverse_index_.size() == queue_.size());
    size_t index_size = 0u;
    for (const RangeList& range_list : index_) {
      index_size += range_list.size();
    }
    DCHECK(index_size == queue_.size());
#endif  // !defined(NDEBUG)
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
  bool Push(T obj, const ui64 hash = 0u) THREAD_SAFE {
    if (closed_) {
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(pop_mutex_);
      if (queue_.size() >= capacity_ && capacity_ != UNLIMITED) {
        return false;
      }
      queue_.push_back(std::move(obj));
      auto last = queue_.end();
      --last;

      PutToIndex(last, hash);
    }
    ++size_;
    pop_condition_.notify_one();

    return true;
  }

  // Returns disengaged object only when this queue is closed and empty.
  Optional Pop(const ui32 range_index = 0u) THREAD_SAFE {
    return Pop(nullptr, range_index);
  }

  Optional Pop(Atomic<ui64>* external_counter,
               const ui32 range_index = 0u) THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_ && queue_.empty()) {
      return Optional();
    }

    auto task = GetFromIndex(range_index);

    Optional&& obj = std::move(*task);
    queue_.erase(task);
    --size_;
    if (external_counter) {
      ++(*external_counter);
    }
    return std::move(obj);
  }

  void UpdateDistribution(const ui32 distribution_range) THREAD_SAFE {
    CHECK(distribution_range > 0u);
    UniqueLock lock(pop_mutex_);
    if (closed_ && queue_.empty()) {
      index_.resize(distribution_range);
      return;
    }
    Vector<RangeList> old_index(index_);
    reverse_index_.clear();
    index_.clear();
    index_.resize(distribution_range);
    chunk_size_ = std::numeric_limits<size_t>::max() / distribution_range;
    for (const RangeList& range_list : old_index) {
      for (const TaskInfo& task_info : range_list) {
        PutToIndex(std::get<ITERATOR>(task_info), std::get<HASH>(task_info));
      }
    }
  }

 private:
  friend class QueueAggregator<T>;

  void PutToIndex(typename Queue::iterator item, const ui64 hash) {
    const ui32 range = static_cast<ui32>(hash / chunk_size_);
    CHECK(range < index_.size()) << "Invalid range for hash / chunk_size "
                                 << hash << "/" << chunk_size_;
    index_[range].emplace_back(std::make_tuple(item, hash));
    typename RangeList::iterator last = index_[range].end();
    --last;

    const T* const item_pointer = &(*item);
    reverse_index_[item_pointer] = std::make_tuple(range, last);
  }

  typename Queue::iterator GetFromIndex(const ui32 range_index) {
    typename Queue::iterator task;
    if (range_index < index_.size() && index_[range_index].size() > 0) {
      task = std::get<ITERATOR>(index_[range_index].front());
    } else {
      task = queue_.begin();
    }
    // Clean up all indexes:
    const T* const task_pointer = &(*task);
    const auto& reverse_info =  reverse_index_[task_pointer];
    RangeList& range_list = index_[std::get<RANGE>(reverse_info)];
    range_list.erase(std::get<REVERSE_ITERATOR>(reverse_info));
    reverse_index_.erase(task_pointer);
    return task;
  }

  std::mutex pop_mutex_;
  std::condition_variable pop_condition_;
  Queue queue_;

  Atomic<ui64> size_ = {0};
  const ui64 capacity_ = UNLIMITED;
  Atomic<bool> closed_ = {false};
  size_t chunk_size_ = {std::numeric_limits<size_t>::max()};
  Vector<RangeList> index_{1};
  // |reverse_index_| is used to remove tasks from |index_| in O(1) when
  // those are picked from the top of the |queue_|.
  HashMap<const T*,
          std::tuple<size_t, typename RangeList::iterator>> reverse_index_;
};

}  // namespace base
}  // namespace dist_clang
