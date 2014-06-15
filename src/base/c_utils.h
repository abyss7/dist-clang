#pragma once

#include <third_party/libcxx/exported/include/cerrno>
#include <third_party/libcxx/exported/include/climits>
#include <third_party/libcxx/exported/include/cstdlib>
#include <third_party/libcxx/exported/include/cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

inline String GetEnv(const char* env_name, const char* default_env = nullptr) {
  const char* env_value = getenv(env_name);
  if (!env_value) {
    if (default_env) {
      return String(default_env);
    }
    return String();
  }
  return String(env_value);
}

inline void GetLastError(String* error) {
  if (error) error->assign(strerror(errno));
}

inline String SetEnv(const char* env_name, const String& value,
                     String* error = nullptr) {
  String old_value = GetEnv(env_name);
  if (setenv(env_name, value.c_str(), 1) == -1) {
    GetLastError(error);
    return String();
  }
  return old_value;
}

inline String GetCurrentDir(String* error = nullptr) {
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof(buf))) {
    GetLastError(error);
    return String();
  }
  return String(buf);
}

inline String CreateTempDir(String* error = nullptr) {
  char buf[] = "/tmp/clangd-XXXXXX";
  if (!mkdtemp(buf)) {
    GetLastError(error);
    return String();
  }
  return String(buf);
}

inline String CreateTempFile(String* error = nullptr) {
  char buf[] = "/tmp/clangd-XXXXXX.files";
  int fd = mkostemps(buf, 6, O_CLOEXEC);
  if (fd == -1) {
    GetLastError(error);
    return String();
  }
  close(fd);
  return String(buf);
}

inline bool ChangeCurrentDir(const String& path, String* error = nullptr) {
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

}  // namespace base
}  // namespace dist_clang
