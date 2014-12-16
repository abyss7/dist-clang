#include <base/file/epoll_linux.h>

#include <base/c_utils.h>

namespace dist_clang {
namespace base {

Epoll::Epoll() : Handle(epoll_create1(EPOLL_CLOEXEC)) {
  if (!IsValid()) {
    GetLastError(&error_);
  }
}

bool Epoll::Add(const Handle& fd, ui32 events, String* error) {
  struct epoll_event event;
  event.events = events;
  event.data.ptr = const_cast<Handle*>(&fd);
  if (epoll_ctl(native(), EPOLL_CTL_ADD, fd.native(), &event) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Epoll::Update(const Handle& fd, ui32 events, String* error) {
  struct epoll_event event;
  event.events = events;
  event.data.ptr = const_cast<Handle*>(&fd);
  if (epoll_ctl(native(), EPOLL_CTL_MOD, fd.native(), &event) == -1) {
    if (errno == ENOENT) {
      if (epoll_ctl(native(), EPOLL_CTL_ADD, fd.native(), &event) == -1) {
        GetLastError(error);
        return false;
      }
    } else {
      GetLastError(error);
      return false;
    }
  }

  return true;
}

bool Epoll::Delete(const Handle& fd, String* error) {
  if (epoll_ctl(native(), EPOLL_CTL_DEL, fd.native(), nullptr) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
