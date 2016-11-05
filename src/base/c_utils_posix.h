#pragma once

#include <base/const_string.h>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(OS_MACOSX)
#include <mach-o/dyld.h>
#endif

namespace dist_clang {
namespace base {

inline Literal SetEnv(Literal env_name, const String& value, String* error) {
  Literal old_value = GetEnv(env_name);
  if (setenv(env_name, value.c_str(), 1) == -1) {
    GetLastError(error);
    return Literal::empty;
  }
  return old_value;
}

inline String CreateTempFile(String* error) {
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

inline String CreateTempFile(const char suffix[], String* error) {
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

inline Literal GetHomeDir(String* error) {
  auto* pw = getpwuid(getuid());
  if (!pw) {
    GetLastError(error);
    return Literal::empty;
  }
  return Literal(pw->pw_dir);
}

inline bool GetSelfPath(String& result, String* error) {
  // TODO: self-path is not a subject to change during the execution. Cache it.
  char path[PATH_MAX];
#if defined(OS_LINUX)
  ssize_t read = readlink("/proc/self/exe", path, sizeof(path));
  if (read == -1) {
    GetLastError(error);
    return false;
  }
  path[read] = '\0';
#elif defined(OS_MACOSX)
  ui32 size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == -1) {
    // TODO: handle not-enough-sized buffer issue.
    if (error) {
      *error = "not enough buffer size";
    }
    return false;
  }
// FIXME: convert path to absolute with |realpath()|.
#else
#pragma message "Don't know how to get self-path on this platform!"
#endif

  result = String(path);
  result = result.substr(0, result.find_last_of('/'));
  return true;
}

inline String NormalizePath(const String& path, String* error) {
  char new_path[PATH_MAX];

  if (realpath(path.c_str(), new_path) == nullptr) {
    GetLastError(error);
    return path;
  }

  return String(new_path);
}

inline bool SetPermissions(const String& path, int mask, String* error) {
  if (chmod(path.c_str(), mask) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

}  // namespace base
}  // namespace dist_clang
