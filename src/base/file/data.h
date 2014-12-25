#pragma once

#if defined(OS_LINUX) || defined(OS_MACOSX)
#include <base/file/handle_posix.h>
#elif defined(OS_WIN)
#include <base/file/handle_win.h>
#endif

namespace dist_clang {
namespace base {

class Data : public Handle {
 public:
  bool MakeBlocking(bool blocking, String* error = nullptr);
  bool IsBlocking() const;

  bool ReadyForRead(int& size, String* error = nullptr) const;

 protected:
  friend class Pipe;

  Data() = default;
  explicit Data(NativeType fd);
};

}  // namespace base
}  // namespace dist_clang
