#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class TemporaryDir {
 public:
  TemporaryDir();
  ~TemporaryDir();

  inline const String& GetError() const { return error_; }
  inline const String str() const { return path_; }

  inline operator bool() const { return path_.empty(); }
  inline operator Path() const { return path_; }

 private:
  Path path_;
  String error_;
};

}  // namespace base
}  // namespace dist_clang
