#include "base/time.h"

#include <mach/clock.h>
#include <mach/mach.h>
#include <time.h>

namespace dist_clang {
namespace base {

bool GetAbsoluteTime(::timespec &time) {
  clock_serv_t cclock;
  mach_timespec_t mts;
  auto res = host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  if (res != KERN_SUCCESS) {
    return false;
  }
  res = clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  if (res != KERN_SUCCESS) {
    return false;
  }
  time.tv_sec = mts.tv_sec;
  time.tv_nsec = mts.tv_nsec;

  return true;
}

}  // namespace base
}  // namespace dist_clang
