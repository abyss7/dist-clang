#pragma once

#include <base/aliases.h>
#include <base/const_string.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(OS_MACOSX)
#include <mach-o/dyld.h>
#endif

namespace dist_clang {
namespace base {

inline Literal GetEnv(Literal env_name, Literal default_env = Literal::empty) {
  Literal env_value = getenv(env_name);
  if (!env_value) {
    if (default_env) {
      return default_env;
    }
    return Literal::empty;
  }
  return env_value;
}

inline void GetLastError(String* error) {
  if (error) {
    error->assign(strerror(errno));
  }
}

inline Literal SetEnv(Literal env_name, const String& value,
                      String* error = nullptr) {
  Literal old_value = GetEnv(env_name);
  if (setenv(env_name, value.c_str(), 1) == -1) {
    GetLastError(error);
    return Literal::empty;
  }
  return old_value;
}

inline Immutable GetCurrentDir(String* error = nullptr) {
  UniquePtr<char[]> buf(new char[PATH_MAX]);
  if (!getcwd(buf.get(), PATH_MAX)) {
    GetLastError(error);
    return Immutable();
  }
  return buf;
}

inline Literal GetHomeDir(String* error = nullptr) {
  auto* pw = getpwuid(getuid());
  if (!pw) {
    GetLastError(error);
    return Literal::empty;
  }
  return Literal(pw->pw_dir);
}

inline String CreateTempFile(String* error = nullptr) {
  char buf[] = "/tmp/clangd-XXXXXX.files";
#if defined(OS_LINUX)
  const int fd = mkostemps(buf, 6, O_CLOEXEC);
#elif defined(OS_MACOSX)
  // FIXME: On MacOSX the temp file isn't closed on exec.
  const int fd = mkstemps(buf, 6);
#else
#error Don't know, how to create a temp file: this platform is unsupported!
#endif
  if (fd == -1) {
    GetLastError(error);
    return String();
  }
  close(fd);
  return String(buf);
}

inline String CreateTempFile(const char suffix[], String* error = nullptr) {
  const char prefix[] = "/tmp/clangd-XXXXXX";
  const size_t prefix_size = sizeof(prefix) - 1;
  UniquePtr<char[]> buf(new char[prefix_size + strlen(suffix) + 1]);
  memcpy(buf.get(), prefix, prefix_size);
  memcpy(buf.get() + prefix_size, suffix, strlen(suffix));
  buf[prefix_size + strlen(suffix)] = 0;

#if defined(OS_LINUX)
  const int fd = mkostemps(buf.get(), strlen(suffix), O_CLOEXEC);
#elif defined(OS_MACOSX)
  // FIXME: On MacOSX the temp file isn't closed on exec.
  const int fd = mkstemps(buf.get(), strlen(suffix));
#else
#error Don't know, how to create a temp file: this platform is unsupported!
#endif
  if (fd == -1) {
    GetLastError(error);
    return String();
  }
  close(fd);
  const auto result = String(buf.get());
  return result;
}

inline bool ChangeCurrentDir(Immutable path, String* error = nullptr) {
  if (chdir(path.c_str()) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool SetPermissions(const String& path, int mask,
                           String* error = nullptr) {
  if (chmod(path.c_str(), mask) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline String GetSelfPath(String* error = nullptr) {
  char path[PATH_MAX];
#if defined(OS_LINUX)
  ssize_t read = readlink("/proc/self/exe", path, PATH_MAX);
  if (read == -1) {
    GetLastError(error);
    return String();
  }
  path[read] = '\0';
#elif defined(OS_MACOSX)
  ui32 size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == -1) {
    // TODO: handle not-enough-sized buffer issue.
  }
// FIXME: convert path to absolute with |realpath()|.
#else
#pragma message "Don't know how to get self-path on this platform!"
#endif

  String&& result = String(path);
  return result.substr(0, result.find_last_of('/'));
}

}  // namespace base
}  // namespace dist_clang
