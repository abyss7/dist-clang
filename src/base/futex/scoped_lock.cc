#include "base/futex/scoped_lock.h"

#include "base/futex/futex_helpers.h"

namespace dist_clang {
namespace base {
namespace futex {

ScopedLock::Guard::Guard()
  : atomic_value_(false), wait_value_(false) {}

ScopedLock::ScopedLock(Guard& guard)
  : guard_(guard) {
  while(true) {
    try_to_wait(guard_.wait_value_, 1);

    bool expected = false;
    if (guard_.atomic_value_.compare_exchange_weak(expected, true)) {
      guard_.wait_value_ = 1;
      break;
    }
  }
}

ScopedLock::~ScopedLock() {
  guard_.wait_value_ = 0;
  guard_.atomic_value_.store(false);
  wake_up(guard_.wait_value_, 1);
}

}  // namespace futex
}  // namespace base
}  // namespace dist_clang
