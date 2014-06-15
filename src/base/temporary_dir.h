#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

class TemporaryDir {
 public:
  TemporaryDir();
  ~TemporaryDir();

  inline const String& GetPath() const;
  inline const String& GetError() const;

  operator String() const;

 private:
  String path_, error_;
};

const String& TemporaryDir::GetPath() const { return path_; }

const String& TemporaryDir::GetError() const { return error_; }

}  // namespace base
}  // namespace dist_clang
