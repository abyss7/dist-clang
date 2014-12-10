#include <base/file/handle_win.h>

#include <base/assert.h>

namespace dist_clang {
namespace base {

Handle::Handle(NativeType fd) : fd_(fd) {
  if (fd == INVALID_HANDLE_VALUE) {
    return;
  }

  // FIXME: check that |fd| is opened.
}

Handle::Handle(Handle&& other) {
  fd_ = other.fd_;
  other.fd_ = INVALID_HANDLE_VALUE;
}

Handle& Handle::operator=(Handle&& other) {
  if (fd_ != INVALID_HANDLE_VALUE) {
    Close();
  }
  fd_ = other.fd_;
  other.fd_ = INVALID_HANDLE_VALUE;

  return *this;
}

Handle::~Handle() {
  if (fd_ == INVALID_HANDLE_VALUE) {
    return;
  }

  Close();
}

void Handle::Close() {
  DCHECK(IsValid());

  CloseHandle(fd_);
  fd_ = INVALID_HANDLE_VALUE;
}

}  // namespace base
}  // namespace dist_clang
