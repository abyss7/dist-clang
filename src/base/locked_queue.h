#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace dist_clang {
namespace base {

template <class T>
class LockedQueue {
  public:
    enum { UNLIMITED = 0 };

    explicit LockedQueue(size_t capacity = UNLIMITED);

    bool Push(T obj);
    bool Pop(T& obj);
    bool Wait();
    bool TryPop(T& obj);
    void Close();
    inline size_t Size() const;

  private:
    std::mutex wait_mutex_;  // TODO: replace with own lockless cond. var.
    std::condition_variable wait_condition_;

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
