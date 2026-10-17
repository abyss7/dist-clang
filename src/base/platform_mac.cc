#include <base/platform.h>

#include <base/c_utils.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <sys/attr.h>
#include <sys/stat.h>
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
  return Platform::MACOSX;
}

i64 ModificationTime(const struct stat& st) {
  return ToNs(st.st_mtimespec);
}

i64 StatusChangeTime(const struct stat& st) {
  return ToNs(st.st_ctimespec);
}

i64 FileSystemTime() {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return ToNs(now);
}

bool SharesStorage(const Path& path, const struct stat& st) {
  if (st.st_nlink > 1) {
    return true;
  }
  // APFS tells the size of the blocks, that aren't shared with the clones.
  struct attrlist attributes = {};
  attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
  attributes.forkattr = ATTR_CMNEXT_PRIVATESIZE;
  struct {
    u_int32_t length;
    off_t private_size;
  } __attribute__((aligned(4), packed)) buffer;
  return getattrlist(path.c_str(), &attributes, &buffer, sizeof(buffer),
                     FSOPT_ATTR_CMN_EXTENDED) == 0 &&
         buffer.private_size < st.st_size;
}

int CreateUniqueFile(char* path_template, int suffix_size) {
  const int fd = mkstemps(path_template, suffix_size);
  if (fd != -1) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  return fd;
}

bool GetSelfPath(String& result, String* error) {
  char path[PATH_MAX];
  ui32 size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == -1) {
    if (error) {
      *error = "not enough buffer size";
    }
    return false;
  }
  // FIXME: convert path to absolute with |realpath()|.
  result = String(path);
  result = result.substr(0, result.find_last_of('/'));
  return true;
}

void SetCurrentThreadName(Literal name) {
  pthread_setname_np(name);
}

void LowerCurrentThreadPriority() {
  // The work, that the user doesn't wait for - on the efficiency cores too.
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}

bool Daemonize(String*) {
  // Unsupported: the function |daemon()| is deprecated - launchd should run the
  // daemon in background.
  return true;
}

}  // namespace base
}  // namespace dist_clang
