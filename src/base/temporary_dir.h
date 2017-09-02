#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class TemporaryDir {
 public:
  TemporaryDir();
  ~TemporaryDir();

  inline const Path& path() const { return path_; }
  inline operator String() const { return path_; }
  inline operator Path() const { return path_; }

 private:
  String error_;
  const Path path_;
};

}  // namespace base
}  // namespace dist_clang
