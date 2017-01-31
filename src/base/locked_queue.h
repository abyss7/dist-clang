#pragma once

#include <base/assert.h>
#include <base/attributes.h>

#include STL(condition_variable)
#include STL(experimental/optional)

namespace dist_clang {
namespace base {

template <class T>
class QueueAggregator;

template <class T>
class LockedQueue {
 public:
  using Optional = std::experimental::optional<T>;
  using Queue = List<T>;

  enum {
    UNLIMITED = 0,
  };

  LockedQueue() : index_(this) {}
  explicit LockedQueue(ui32 capacity) : capacity_(capacity), index_(this) {}
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

      index_.Put(last, hash);
    }
    ++size_;
    pop_condition_.notify_one();

    return true;
  }

  // Returns disengaged object only when this queue is closed and empty.
  Optional Pop(const ui32 shard = 0u) THREAD_SAFE {
    return Pop(nullptr, shard);
  }

  Optional Pop(Atomic<ui64>* external_counter,
               const ui32 shard = 0u) THREAD_SAFE {
    UniqueLock lock(pop_mutex_);
    pop_condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (closed_ && queue_.empty()) {
      return Optional();
    }

    auto task = index_.Get(shard);

    Optional&& obj = std::move(*task);
    queue_.erase(task);
    --size_;
    if (external_counter) {
      ++(*external_counter);
    }
    return std::move(obj);
  }

  void UpdateIndex(const ui32 shards_number) THREAD_SAFE {
    CHECK(shards_number > 0u);
    UniqueLock lock(pop_mutex_);
    index_.Update(shards_number);
  }

 private:
  friend class QueueAggregator<T>;

  class Index {
   public:
    using TaskInfo = Tuple<typename Queue::iterator, size_t>;
    using ShardsList = List<TaskInfo>;  // List of tasks from same shard.
    enum InfoIndex {
      ITERATOR = 0u,
      HASH = 1u,
    };
    enum ReverseInfoIndex {
      RANGE = 0u,
      REVERSE_ITERATOR = 1u,
    };

    Index(LockedQueue<T>* const queue) : queue_(queue) {
      CHECK(queue_) << "Queue must be specified";
    }

    ~Index() {
      // Make sure we don't leak tasks.
      DCHECK(reverse_index_.size() == Size());
    }

    size_t Size() const THREAD_UNSAFE {
      size_t index_size = 0u;
      for (const ShardsList& shards_list : index_) {
        index_size += shards_list.size();
      }
      return index_size;
    }

    void Put(typename Queue::iterator item, const ui64 hash) {
      const ui32 shard = static_cast<ui32>(hash / chunk_size_);
      CHECK(shard < index_.size()) << "Invalid shard for hash / chunk_size "
                                   << hash << "/" << chunk_size_;
      index_[shard].emplace_back(std::make_tuple(item, hash));
      typename ShardsList::iterator last = index_[shard].end();
      --last;

      const T* const item_pointer = &(*item);
      reverse_index_[item_pointer] = std::make_tuple(shard, last);
    }

    typename Queue::iterator Get(const ui32 shard) {
      typename Queue::iterator task;
      if (shard < index_.size() && index_[shard].size() > 0) {
        task = std::get<ITERATOR>(index_[shard].front());
      } else {
        task = queue_->queue_.begin();
      }
      // Clean up all indexes:
      const T* const task_pointer = &(*task);
      const auto& reverse_info =  reverse_index_[task_pointer];
      ShardsList& shards_list = index_[std::get<RANGE>(reverse_info)];
      shards_list.erase(std::get<REVERSE_ITERATOR>(reverse_info));
      reverse_index_.erase(task_pointer);
      return task;
    }

    void Update(const ui32 shards_number) THREAD_UNSAFE {
      if (queue_->closed_ && queue_->queue_.empty()) {
        index_.resize(shards_number);
        return;
      }
      Vector<ShardsList> old_index(index_);
      reverse_index_.clear();
      index_.clear();
      index_.resize(shards_number);
      chunk_size_ = std::numeric_limits<size_t>::max() / shards_number;
      for (const ShardsList& shards_list : old_index) {
        for (const TaskInfo& task_info : shards_list) {
          Put(std::get<ITERATOR>(task_info), std::get<HASH>(task_info));
        }
      }
    }

   private:
    LockedQueue<T>* const queue_;
    size_t chunk_size_ = {std::numeric_limits<size_t>::max()};
    Vector<ShardsList> index_{1};
    // |reverse_index_| is used to remove tasks from |index_| in O(1) when
    // those are picked from the top of the |queue_|.
    HashMap<const T*,
            std::tuple<size_t, typename ShardsList::iterator>> reverse_index_;
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
