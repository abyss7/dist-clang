#pragma once

#include <base/file/handle_posix.h>

#include STL(array)

#include <sys/event.h>

namespace dist_clang {
namespace base {

class Kqueue final : public Handle {
 public:
  enum : int { UNLIMITED = -1 };

  Kqueue();

  bool Add(const Handle& fd, ui32 events, String* error = nullptr);
  bool Update(const Handle& fd, ui32 events, String* error = nullptr);
  bool Delete(const Handle& fd, String* error = nullptr);

  template <size_t array_size>
  int Wait(std::array<struct kevent, array_size>& events, int sec_timeout) {
    if (sec_timeout == UNLIMITED) {
      return kevent(native(), nullptr, 0, events.data(), array_size, nullptr);
    } else {
      struct timespec timeout = {sec_timeout, 0};
      struct timespec* timeout_ptr = &timeout;
      return kevent(native(), nullptr, 0, events.data(), array_size,
                    timeout_ptr);
    }
  }

  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

 private:
  String error_;
};

}  // namespace base
}  // namespace dist_clang
