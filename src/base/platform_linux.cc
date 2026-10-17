#include <base/platform.h>

#include <base/c_utils.h>

#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

namespace {

i64 ToNs(const struct timespec& time) {
  return i64(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

}  // namespace

Platform CurrentPlatform() {
  return Platform::LINUX;
}

i64 ModificationTime(const struct stat& st) {
  return ToNs(st.st_mtim);
}

i64 StatusChangeTime(const struct stat& st) {
  return ToNs(st.st_ctim);
}

i64 FileSystemTime() {
  // The file systems take the time of modifications from the coarse clock.
  struct timespec now;
  clock_gettime(CLOCK_REALTIME_COARSE, &now);
  return ToNs(now);
}

int CreateUniqueFile(char* path_template, int suffix_size) {
  return mkostemps(path_template, suffix_size, O_CLOEXEC);
}

bool GetSelfPath(String& result, String* error) {
  char path[PATH_MAX];
  const ssize_t size = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (size == -1) {
    GetLastError(error);
    return false;
  }
  result.assign(path, size);
  result = result.substr(0, result.find_last_of('/'));
  return true;
}

void SetCurrentThreadName(Literal name) {
  pthread_setname_np(pthread_self(), name);
}

void LowerCurrentThreadPriority() {
  setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10);
}

bool Daemonize(String* error) {
  if (::daemon(0, 1) != 0) {
    GetLastError(error);
    return false;
  }
  return true;
}

}  // namespace base
}  // namespace dist_clang
