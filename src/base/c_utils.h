#pragma once

#include <base/aliases.h>
#include <base/const_string.h>

#include <base/c_utils_forward.h>
#if defined(OS_WIN)
#include <base/c_utils_win.h>
#elif defined(OS_LINUX) || defined(OS_MACOSX)
#include <base/c_utils_posix.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include STL_EXPERIMENTAL(filesystem)
#include STL(system_error)

namespace dist_clang {
namespace base {

inline bool ChangeCurrentDir(const Path& path, String* error) {
  std::error_code ec;
  std::experimental::filesystem::current_path(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

inline Path GetCurrentDir(String* error) {
  std::error_code ec;
  const auto& current_dir = std::experimental::filesystem::current_path(ec);
  if (ec && error) {
    *error = ec.message();
  }
  return current_dir;
}

inline Literal GetEnv(Literal env_name, Literal default_env) {
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

inline bool SetPermissions(const Path& path, const Perms permissions,
                           String* error) {
  std::error_code ec;
  std::experimental::filesystem::permissions(path.c_str(), permissions, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}


}  // namespace base
}  // namespace dist_clang
