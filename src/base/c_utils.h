#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

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

inline std::string GetCurrentDir(std::string* error = nullptr) {
  char buf[PATH_MAX];
  if (!getcwd(buf, sizeof(buf))) {
    GetLastError(error);
    return std::string();
  }
  return std::string(buf);
}

inline std::string CreateTempDir(std::string* error = nullptr) {
  char buf[] = "/tmp/socket-XXXXXX";
  if (!mkdtemp(buf)) {
    GetLastError(error);
    return std::string();
  }
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

}  // namespace base
}  // namespace dist_clang
