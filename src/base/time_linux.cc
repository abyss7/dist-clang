#include "base/time.h"

#include <time.h>

namespace dist_clang {
namespace base {

bool GetAbsoluteTime(::timespec &time) {
  return clock_gettime(CLOCK_MONOTONIC, &time) != -1;
}

}  // namespace base
}  // namespace dist_clang
