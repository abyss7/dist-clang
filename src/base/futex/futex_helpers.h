#pragma once

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dist_clang {
namespace base {
namespace futex {

enum WaitResult { WAKE_UP, BAD_VALUE, ERROR };

inline long futex(const void *addr1, int op, int val1, struct timespec *timeout,
                  const void *addr2, int val3) {
  return syscall(SYS_futex, addr1, op, val1, timeout, addr2, val3);
}

template <typename T>
inline WaitResult try_to_wait(const T& variable, T value) {
  if (futex(&variable, FUTEX_WAIT, value, nullptr, nullptr, 0) == -1) {
    if (errno == EWOULDBLOCK)
      return BAD_VALUE;
    else
      return ERROR;
  }
  return WAKE_UP;
}

template <typename T>
inline size_t wake_up(const T& variable, size_t threads_num) {
  int res = futex(&variable, FUTEX_REQUEUE, threads_num, nullptr, &variable, 0);
  if (res < 0)
    return 0;
  return static_cast<size_t>(res);
}

}  // namespace futex
}  // namespace base
}  // namespace dist_clang
