#include <base/file/kqueue_mac.h>

#include <base/c_utils.h>

namespace dist_clang {
namespace base {

Kqueue::Kqueue() : Handle(kqueue()) {
  if (!IsValid()) {
    GetLastError(&error_);
  }
}

bool Kqueue::Add(const Handle& fd, ui32 events, String* error) {
  struct kevent event;
  EV_SET(&event, fd.native(), events, EV_ADD, 0, 0, const_cast<Handle*>(&fd));
  if (kevent(native(), &event, 1, nullptr, 0, nullptr) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Kqueue::Update(const Handle& fd, ui32 events, String* error) {
  struct kevent event;
  EV_SET(&event, fd.native(), events, EV_ADD | EV_ONESHOT, 0, 0,
         const_cast<Handle*>(&fd));
  if (kevent(native(), &event, 1, nullptr, 0, nullptr) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

bool Kqueue::Delete(const Handle& fd, String* error) {
  struct kevent event;
  EV_SET(&event, fd.native(), EVFILT_READ, EV_DELETE, 0, 0, 0);
  if (kevent(native(), &event, 1, nullptr, 0, nullptr) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
