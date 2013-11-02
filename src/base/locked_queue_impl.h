#pragma once

#include "base/locked_queue.h"

namespace dist_clang {
namespace base {

template <class T>
LockedQueue<T>::LockedQueue(size_t capacity)
  : size_(0), capacity_(capacity), closed_(false) {
}

template <class T>
void LockedQueue<T>::Close() {
  bool old_closed = false;
  if (closed_.compare_exchange_strong(old_closed, true)) {
    pop_condition_.notify_all();
  }
}

template <class T>
bool LockedQueue<T>::Push(T obj) {
  if (closed_.load()) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(pop_mutex_);
    if (queue_.size() >= capacity_ && capacity_ != UNLIMITED) {
      return false;
    }
    queue_.push(std::move(obj));
  }
  size_.fetch_add(1);
  pop_condition_.notify_one();

  return true;
}

template <class T>
bool LockedQueue<T>::Pop(T& obj) {
  std::unique_lock<std::mutex> lock(pop_mutex_);
  pop_condition_.wait(lock, [this]{ return closed_ || !queue_.empty(); });
  if (closed_ && queue_.empty()) {
    return false;
  }
  obj = std::move(queue_.front());
  queue_.pop();
  size_.fetch_sub(1);
  return true;
}

}  // namespace base
}  // namespace dist_clang
