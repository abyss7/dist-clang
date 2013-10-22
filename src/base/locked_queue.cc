#include "base/locked_queue.h"

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
  wait_condition_.notify_all();
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
  wait_condition_.notify_all();
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
bool LockedQueue<T>::Wait() {
  std::unique_lock<std::mutex> lock(wait_mutex_);
  while (!closed_ && !size_.load()) {
    wait_condition_.wait(lock);
  }
  if (closed_ && !size_.load()) {
    return false;
  }
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
