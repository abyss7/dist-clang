#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

inline std::string GetEnv(const char* env_name,
                          const char* default_env = nullptr) {
  const char* env_value = getenv(env_name);
  if (!env_value) {
    if (default_env)
      return std::string(default_env);
    return std::string();
  }
  return std::string(env_value);
}

inline void GetLastError(std::string* error) {
  if (error)
    error->assign(strerror(errno));
}

inline std::string SetEnv(const char* env_name, const std::string& value,
                          std::string* error = nullptr) {
  std::string old_value = GetEnv(env_name);
  if (setenv(env_name, value.c_str(), 1) == -1) {
    GetLastError(error);
    return std::string();
  }
  return old_value;
}

inline std::string GetCurrentDir(std::string* error = nullptr) {
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof(buf))) {
    GetLastError(error);
    return std::string();
  }
  return std::string(buf);
}

inline std::string CreateTempDir(std::string* error = nullptr) {
  char buf[] = "/tmp/clangd-XXXXXX";
  if (!mkdtemp(buf)) {
    GetLastError(error);
    return std::string();
  }
  return std::string(buf);
}

inline std::string CreateTempFile(std::string* error = nullptr) {
  char buf[] = "/tmp/clangd-XXXXXX.files";
  int fd = mkostemps(buf, 6, O_CLOEXEC);
  if (fd == -1) {
    GetLastError(error);
    return std::string();
  }
  close(fd);
  return std::string(buf);
}

inline bool ChangeCurrentDir(const std::string& path,
                             std::string* error = nullptr) {
  if (chdir(path.c_str()) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool SetPermissions(const std::string& path, int mask,
                           std::string* error = nullptr) {
  if (chmod(path.c_str(), mask) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

}  // namespace base
}  // namespace dist_clang
