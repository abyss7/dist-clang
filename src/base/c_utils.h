#pragma once

#include <base/const_string.h>
#include <base/types.h>

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

namespace dist_clang {
namespace base {

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

}  // namespace base
}  // namespace dist_clang
