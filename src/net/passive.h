#pragma once

#if defined(OS_LINUX) || defined(OS_MACOSX)
#include <base/file/handle_posix.h>
#elif defined(OS_WIN)
#include <base/file/handle_win.h>
#endif

namespace dist_clang {
namespace net {

class Socket;

class Passive final : public base::Handle {
 public:
  explicit Passive(Socket&& fd, ui32 backlog = 64);

  Socket Accept();

  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

  inline bool IsValid() const {
    if (error_.empty()) {
      return Handle::IsValid();
    }
    return false;
  }

 private:
  String error_;
};

}  // namespace net
}  // namespace dist_clang

namespace std {

template <>
struct hash<dist_clang::net::Passive> {
 public:
  size_t operator()(const dist_clang::net::Passive& value) const {
    return std::hash<dist_clang::net::Passive::NativeType>()(value.native());
  }
};

}  // namespace std
