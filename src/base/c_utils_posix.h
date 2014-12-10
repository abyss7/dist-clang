#pragma once

#include <pwd.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

inline Literal SetEnv(Literal env_name, const String& value,
                      String* error = nullptr) {
  Literal old_value = GetEnv(env_name);
  if (setenv(env_name, value.c_str(), 1) == -1) {
    GetLastError(error);
    return Literal::empty;
  }
  return old_value;
}

}  // namespace base
}  // namespace dist_clang
