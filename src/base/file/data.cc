#include <base/file/data.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include <fcntl.h>
#include <sys/ioctl.h>

namespace dist_clang {
namespace base {

Data::Data(NativeType fd) : Handle(fd) {
  DCHECK(!IsPassive());
}

bool Data::MakeBlocking(bool blocking, String* error) {
  DCHECK(IsValid());

  int flags = fcntl(native(), F_GETFL, 0);
  if (flags == -1) {
    GetLastError(error);
    return false;
  }

  if (!blocking) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }

  int res = fcntl(native(), F_SETFL, flags);
  if (res == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Data::IsBlocking() const {
  DCHECK(IsValid());

  int flags = fcntl(native(), F_GETFL, 0);
  if (flags == -1) {
    return false;
  }

  return !(flags & O_NONBLOCK);
}

bool Data::ReadyForRead(int& size, String* error) const {
  DCHECK(IsValid());

  if (ioctl(native(), FIONREAD, &size) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
