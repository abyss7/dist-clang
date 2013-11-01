#pragma once

#include "base/attributes.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_set>

namespace dist_clang {
namespace base {

class LockedQueueObserver;

template <class T>
class LockedQueue {
  public:
    enum { UNLIMITED = 0 };

    explicit LockedQueue(size_t capacity = UNLIMITED);
    void Close() THREAD_SAFE;

    void AddObserver(LockedQueueObserver* observer);
    void RemoveObserver(LockedQueueObserver* observer);

    bool Push(T obj) THREAD_SAFE;
    bool Pop(T& obj) THREAD_SAFE;
    bool TryPop(T& obj) THREAD_SAFE;
    inline size_t Size() const THREAD_SAFE;

  private:
    std::mutex observer_mutex_;
    std::unordered_set<LockedQueueObserver*> observers_;

    std::mutex pop_mutex_;
    std::condition_variable pop_condition_;
    std::queue<T> queue_;

    std::atomic<size_t> size_;
    size_t capacity_;
    bool closed_;
};

template <class T>
size_t LockedQueue<T>::Size() const {
  return size_.load();
}

}  // namespace base
}  // namespace dist_clang
