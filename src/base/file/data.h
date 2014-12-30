#pragma once

#if defined(OS_LINUX) || defined(OS_MACOSX)
#include <base/file/handle_posix.h>
#elif defined(OS_WIN)
#include <base/file/handle_win.h>
#endif

namespace dist_clang {

namespace net {
class EpollEventLoop;
}

namespace base {

class Data : public Handle {
 public:
  bool MakeBlocking(bool blocking, String* error = nullptr);
  bool IsBlocking() const;

  bool Read(Immutable* output, String* error = nullptr);

 protected:
  friend class File;                 // Data(NativeType)
  friend class Pipe;                 // Data()
  friend class net::EpollEventLoop;  // ReadyForRead(...)

  Data() = default;
  explicit Data(NativeType fd);

  bool ReadyForRead(int& size, String* error = nullptr) const;
};

}  // namespace base
}  // namespace dist_clang
