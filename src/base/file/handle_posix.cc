#include <base/file/handle_posix.h>

#include <base/c_utils.h>
#include <base/file/handle_tracking.h>

#include <sys/socket.h>

namespace dist_clang {
namespace base {


Handle::Handle(NativeType fd) : fd_(fd) {
  if (fd == -1) {
    return;
  }

  DCHECK(fd >= 0);

// FIXME: check that |fd| is opened.

  TrackHandle(fd_);
}

Handle::Handle(Handle&& other) {
  fd_ = other.fd_;
  other.fd_ = -1;
}

Handle& Handle::operator=(Handle&& other) {
  if (fd_ != -1) {
    Close();
  }
  fd_ = other.fd_;
  other.fd_ = -1;

  return *this;
}

Handle::~Handle() {
  if (fd_ == -1) {
    return;
  }

  Close();
}

bool Handle::CloseOnExec(String* error) {
  DCHECK(IsValid());

  if (fcntl(fd_, F_SETFD, FD_CLOEXEC) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

// static
Handle& Handle::stdin() {
  static Handle stdin(STDIN_FILENO);
  return stdin;
}

// static
Handle& Handle::stdout() {
  static Handle stdout(STDOUT_FILENO);
  return stdout;
}

// static
Handle& Handle::stderr() {
  static Handle stderr(STDERR_FILENO);
  return stderr;
}

bool Handle::Duplicate(Handle&& other, String* error) {
  DCHECK(IsValid());

  if (dup2(fd_, other.fd_) == -1) {
    GetLastError(error);
    return false;
  }

  Close();
  fd_ = other.fd_;
  other.fd_ = -1;

  return true;
}

void Handle::Close() {
  DCHECK(IsValid());
  UntrackHandle(fd_);

  close(fd_);
  fd_ = -1;
}

bool Handle::IsPassive() const {
  DCHECK(IsValid());

  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd_, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

}  // namespace base
}  // namespace dist_clang
