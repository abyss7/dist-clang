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
  // The file systems take the times of the modifications from the coarse
  // clock - it's behind the precise one up to a tick - but the newer kernels
  // make them precise, when they are seen. The time a bit earlier counts the
  // modifications after it in both cases - and the ones shortly before it
  // too: they only don't let to cache the result.
  // The coarse clock lags up to about two ticks.
  static const i64 kMaxLag = [] {
    struct timespec tick;
    if (clock_getres(CLOCK_REALTIME_COARSE, &tick) != 0) {
      return i64(10'000'000);
    }
    return 2 * ToNs(tick) + 1'000'000;
  }();
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return ToNs(now) - kMaxLag;
}

bool SharesStorage(const Path&, const struct stat& st) {
  // TODO: the shared extents of the clones - on Btrfs and XFS.
  return st.st_nlink > 1;
}

String PathKey(const String& path) {
  return path;
}

int CreateUniqueFile(char* path_template, int suffix_size) {
  return mkostemps(path_template, suffix_size, O_CLOEXEC);
}

bool GetSelfExecutable(String& result, String* error) {
  char path[PATH_MAX];
  const ssize_t size = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (size == -1) {
    GetLastError(error);
    return false;
  }
  result.assign(path, size);
  return true;
}

bool GetSelfPath(String& result, String* error) {
  if (!GetSelfExecutable(result, error)) {
    return false;
  }
  result = result.substr(0, result.find_last_of('/'));
  return true;
}

void SetCurrentThreadName(Literal name) {
  pthread_setname_np(pthread_self(), name);
}

void LowerCurrentThreadPriority() {
  setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10);
}

bool RaiseOpenFilesLimit(String* error) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
    GetLastError(error);
    return false;
  }
  const rlim_t wanted = limit.rlim_max;
  if (limit.rlim_cur >= wanted) {
    return true;
  }
  limit.rlim_cur = wanted;
  if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
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
