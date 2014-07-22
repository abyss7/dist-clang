#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class FileHolder {
 public:
  FileHolder() = default;
  FileHolder(const String& path);

  inline operator bool() const { return impl_ && impl_->fd != -1; }

  inline String GetPath() const { return impl_ ? impl_->path : String(); }
  inline int GetDescriptor() const { return impl_ ? impl_->fd : -1; }
  inline String GetError() const { return impl_ ? impl_->error : String(); }

 private:
  struct Internals {
    Internals(const String& path);
    ~Internals();

    const String path;
    const int fd = -1;
    String error;
  };

  SharedPtr<Internals> impl_;
};

}  // namespace base
}  // namespace dist_clang
