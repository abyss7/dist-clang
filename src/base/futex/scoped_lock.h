#pragma once

#include <atomic>

namespace dist_clang {
namespace base {
namespace futex {

class ScopedLock {
  public:
    class Guard {
      public:
        Guard();

      private:
        friend class ScopedLock;

        std::atomic<bool> atomic_value_;
        int wait_value_;
    };

    explicit ScopedLock(Guard &guard);
    ~ScopedLock();

  private:
    Guard& guard_;
};

}  // namespace futex
}  // namespace base
}  // namespace dist_clang
