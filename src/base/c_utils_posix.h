#pragma once

#include <limits.h>

#include <base/const_string.h>
#include <base/platform.h>

#include <fcntl.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  const int fd = CreateUniqueFile(buf, 6);
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

  const int fd = CreateUniqueFile(buf.get(), strlen(suffix));
  if (fd == -1) {
    GetLastError(error);
    return String();
  }
  close(fd);
  const auto result = String(buf.get());
  return result;
}

inline Path GetHomeDir(String* error) {
  auto* pw = getpwuid(getuid());
  if (!pw) {
    GetLastError(error);
    return Path();
  }
  return Path(pw->pw_dir);
}

// TODO: refactor on |std::filesystem|.
inline bool SetPermissions(const String& path, int mask, String* error) {
  if (chmod(path.c_str(), mask) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

}  // namespace base
}  // namespace dist_clang
