#pragma once

#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

template <typename T>
inline T Random() {
  T output;

  auto fd = open("/dev/urandom", O_RDONLY);
  if (fd == -1)
    output = static_cast<T>(random());
  else {
    if (read(fd, &output, sizeof(output)) == -1)
      output = static_cast<T>(random());
    close(fd);
  }

  return output;
}

}  // namespace base
}  // namespace dist_clang
