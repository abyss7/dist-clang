#include <base/file/handle_tracking.h>

#include <base/assert.h>
#include <base/file/handle_posix.h>

#include STL(bitset)
#include STL(limits)
#include STL(mutex)

#include <pthread.h>

namespace dist_clang {
namespace base {

#if !defined(NDEBUG)
namespace {

std::mutex used_fds_mutex;
std::bitset<std::numeric_limits<Handle::NativeType>::max()> used_fds;

void before_fork() {
  used_fds_mutex.lock();
}

void after_fork() {
  used_fds_mutex.unlock();
}

int atfork_result = pthread_atfork(before_fork, after_fork, after_fork);

}  // namespace

void TrackHandle(int fd) {
  UniqueLock lock(used_fds_mutex);
  DCHECK(!used_fds[fd]);
  used_fds[fd] = true;
}

void UntrackHandle(int fd) {
  UniqueLock lock(used_fds_mutex);
  DCHECK(used_fds[fd]);
  used_fds[fd] = false;
}
#else
void TrackHandle(int) {}
void UntrackHandle(int) {}
#endif  // !defined(NDEBUG)

}  // namespace base
}  // namespace dist_clang
