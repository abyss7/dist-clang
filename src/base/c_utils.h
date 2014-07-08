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
  if (error) {
    error->assign(strerror(errno));
  }
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

inline String CreateTempFile(const char suffix[], String* error = nullptr) {
  const char prefix[] = "/tmp/clangd-XXXXXX";
  const size_t prefix_size = sizeof(prefix) - 1;
  char* buf = new char[prefix_size + strlen(suffix) + 1];
  memcpy(&buf[0], prefix, prefix_size);
  memcpy(&buf[prefix_size], suffix, strlen(suffix));
  buf[prefix_size + strlen(suffix)] = 0;

  int fd = mkostemps(buf, strlen(suffix), O_CLOEXEC);
  if (fd == -1) {
    GetLastError(error);
    delete[] buf;
    return String();
  }
  close(fd);
  auto result = String(buf);
  delete[] buf;
  return result;
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

inline String GetSelfPath(String* error = nullptr) {
  // FIXME: insert MAX_PATH constant.
  char path[1024];
  ssize_t r;

  r = readlink("/proc/self/exe", path, 1024);
  if (r == -1) {
    GetLastError(error);
    return String();
  }

  path[r] = '\0';
  String&& result = String(path);
  return result.substr(0, result.find_last_of('/'));
}

}  // namespace base
}  // namespace dist_clang
