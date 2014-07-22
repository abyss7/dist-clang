#include <base/file_holder.h>

#include <base/c_utils.h>

#include <fcntl.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

FileHolder::FileHolder(const String& path) : impl_(new Internals(path)) {}

FileHolder::Internals::Internals(const String& path)
    : path(path), fd(open(path.c_str(), O_RDONLY)) {
  if (fd == -1) {
    GetLastError(&error);
  }
}

FileHolder::Internals::~Internals() {
  if (fd != -1) {
    close(fd);
  }
}

}  // namespace base
}  // namespace dist_clang
