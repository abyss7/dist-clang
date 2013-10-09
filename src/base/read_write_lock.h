#pragma once

#include <condition_variable>
#include <mutex>

namespace dist_clang {
namespace base {

class ReadWriteMutex {
  public:
    ReadWriteMutex() : writing_(false), readers_(0) {}

  private:
    friend class ReadLock;
    friend class WriteLock;

    void read_lock() {
      std::unique_lock<std::mutex> lock(mutex_);
      while (writing_) {
        cond_var_.wait(lock);
      }
      readers_++;
    }

    void read_unlock() {
      std::unique_lock<std::mutex> lock(mutex_);
      readers_--;
      if (!readers_) {
        cond_var_.notify_all();
      }
    }

    void write_lock() {
      std::unique_lock<std::mutex> lock(mutex_);
      while (writing_ || readers_) {
        cond_var_.wait(lock);
      }
      writing_ = true;
    }

    void write_unlock() {
      std::unique_lock<std::mutex> lock(mutex_);
      writing_ = false;
      cond_var_.notify_all();
    }

    bool writing_;
    size_t readers_;
    std::condition_variable cond_var_;
    std::mutex mutex_;
};

class ReadLock {
  public:
    ReadLock(ReadWriteMutex& mutex)
      : mutex_(mutex) {
      mutex_.read_lock();
    }
    ~ReadLock() {
      mutex_.read_unlock();
    }

  private:
    ReadWriteMutex& mutex_;
};

class WriteLock {
  public:
    WriteLock(ReadWriteMutex& mutex)
      : mutex_(mutex) {
      mutex_.write_lock();
    }
    ~WriteLock() {
      mutex_.write_unlock();
    }

  private:
    ReadWriteMutex& mutex_;
};

}  // namespace base
}  // namespace dist_clang
