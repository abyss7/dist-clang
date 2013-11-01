#include "base/locked_queue.h"

#include "base/locked_queue_observer.h"

namespace dist_clang {
namespace base {

template <class T>
LockedQueue<T>::LockedQueue(size_t capacity)
  : size_(0), capacity_(capacity), closed_(false) {
}

template <class T>
void LockedQueue<T>::Close() {
  closed_ = true;
  pop_condition_.notify_all();

  {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* observer: observers_) {
      observer->Observe(true);
    }
  }
}

template <class T>
void LockedQueue<T>::AddObserver(LockedQueueObserver* observer) {
  std::lock_guard<std::mutex> lock(observer_mutex_);
  observers_.insert(observer);
}

template <class T>
void LockedQueue<T>::RemoveObserver(LockedQueueObserver* observer) {
  std::lock_guard<std::mutex> lock(observer_mutex_);
  observers_.erase(observer);
}

template <class T>
bool LockedQueue<T>::Push(T obj) {
  {
    std::lock_guard<std::mutex> lock(pop_mutex_);
    if (queue_.size() >= capacity_ && capacity_ != UNLIMITED && !closed_) {
      return false;
    }
    queue_.push(std::move(obj));
  }
  size_.fetch_add(1);
  pop_condition_.notify_one();

  {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* observer: observers_) {
      observer->Observe(false);
    }
  }
  return true;
}

template <class T>
bool LockedQueue<T>::Pop(T& obj) {
  std::unique_lock<std::mutex> lock(pop_mutex_);
  while(!closed_ && queue_.empty()) {
    pop_condition_.wait(lock);
  }
  if (closed_ && queue_.empty()) {
    return false;
  }
  obj = std::move(queue_.front());
  queue_.pop();
  size_.fetch_sub(1);
  return true;
}

template <class T>
bool LockedQueue<T>::TryPop(T& obj) {
  std::unique_lock<std::mutex> lock(pop_mutex_);
  if (queue_.empty()) {
    return false;
  }
  obj = std::move(queue_.front());
  queue_.pop();
  size_.fetch_sub(1);
  return true;
}

}  // namespace base
}  // namespace dist_clang
