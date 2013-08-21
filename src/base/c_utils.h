#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

namespace {

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

inline std::string GetCurrentDir() {
  char buf[PATH_MAX];
  if (!::getcwd(buf, sizeof(buf)))
    return std::string();
  return std::string(buf);
}

}  // namespace
