#include <base/file/handle_posix.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include STL(bitset)
#include STL(limits)

#include <pthread.h>
#include <sys/socket.h>

namespace dist_clang {
namespace base {

namespace {

std::mutex used_fds_mutex;
std::bitset<std::numeric_limits<Handle::NativeType>::max()> used_fds;

void before_fork() {
  used_fds_mutex.lock();
}

void after_fork() {
  used_fds_mutex.unlock();
}

int atfork_result = pthread_atfork(before_fork, after_fork, after_fork);

}  // namespace

Handle::Handle(NativeType fd) : fd_(fd) {
  if (fd == -1) {
    return;
  }

  DCHECK(fd >= 0);

  // FIXME: check that |fd| is opened.
  UniqueLock lock(used_fds_mutex);
  DCHECK(!used_fds[fd_]);
  used_fds[fd_] = true;
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
  UniqueLock lock(used_fds_mutex);

  DCHECK(IsValid());
  DCHECK(used_fds[fd_]);

  used_fds[fd_] = false;
  close(fd_);
  fd_ = -1;
}

bool Handle::IsPassive() const {
  int res;
  socklen_t size = sizeof(res);
  return getsockopt(fd_, SOL_SOCKET, SO_ACCEPTCONN, &res, &size) != -1 && res;
}

}  // namespace base
}  // namespace dist_clang
