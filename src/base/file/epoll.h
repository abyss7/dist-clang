#pragma once

#include <base/file/handle_posix.h>

#include STL(array)

#include <sys/epoll.h>

namespace dist_clang {
namespace base {

class Epoll final : public Handle {
 public:
  Epoll();

  bool Add(const Handle& fd, ui32 events, String* error = nullptr);
  bool Update(const Handle& fd, ui32 events, String* error = nullptr);
  bool Delete(const Handle& fd, String* error = nullptr);

  template <size_t array_size>
  int Wait(std::array<struct epoll_event, array_size>& events, int timeout) {
    return epoll_wait(native(), events.data(), array_size, timeout);
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
