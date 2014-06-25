#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class TemporaryDir {
 public:
  TemporaryDir();
  ~TemporaryDir();

  inline const String& GetPath() const { return path_; }
  inline const String& GetError() const { return error_; }

  inline operator bool() const { return !path_.empty(); }
  inline operator String() const { return path_; }

 private:
  String path_, error_;
};

}  // namespace base
}  // namespace dist_clang
