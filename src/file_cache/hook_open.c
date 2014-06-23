#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/types.h>

int open(const char *pathname, int flags, ...) {
  int has_mode = 0;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    has_mode = 1;
    va_end(ap);
  }

  int (*next)(const char * pathname, int flags, ...);
  next = dlsym(RTLD_NEXT, "open");

  // TODO: save opened file to manifest.

  if (has_mode) {
    return next(pathname, flags, mode);
  } else {
    return next(pathname, flags);
  }
}

int creat(const char *pathname, mode_t mode) {
  int (*next)(const char * pathname, mode_t mode);
  next = dlsym(RTLD_NEXT, "creat");

  // TODO: save opened file to manifest.

  return next(pathname, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
  int has_mode = 0;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    has_mode = 1;
    va_end(ap);
  }

  int (*next)(int dirfd, const char * pathname, int flags, ...);
  next = dlsym(RTLD_NEXT, "openat");

  // TODO: save opened file to manifest.

  if (has_mode) {
    return next(dirfd, pathname, flags, mode);
  } else {
    return next(dirfd, pathname, flags);
  }
}

#ifdef __USE_LARGEFILE64
int open64(const char *pathname, int flags, ...) {
  int has_mode = 0;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    has_mode = 1;
    va_end(ap);
  }

  int (*next)(const char * pathname, int flags, ...);
  next = dlsym(RTLD_NEXT, "open64");

  // TODO: save opened file to manifest.

  if (has_mode) {
    return next(pathname, flags, mode);
  } else {
    return next(pathname, flags);
  }
}

int creat64(const char *pathname, mode_t mode) {
  int (*next)(const char * pathname, mode_t mode);
  next = dlsym(RTLD_NEXT, "creat64");

  // TODO: save opened file to manifest.

  return next(pathname, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
  int has_mode = 0;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    has_mode = 1;
    va_end(ap);
  }

  int (*next)(int dirfd, const char * pathname, int flags, ...);
  next = dlsym(RTLD_NEXT, "openat64");

  // TODO: save opened file to manifest.

  if (has_mode) {
    return next(dirfd, pathname, flags, mode);
  } else {
    return next(dirfd, pathname, flags);
  }
}
#endif  // __USE_LARGEFILE64
