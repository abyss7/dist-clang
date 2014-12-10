#pragma once

#include <base/file/data.h>

namespace dist_clang {
namespace base {

class Pipe {
 public:
  explicit Pipe(bool blocking = true);

  Data& operator[](ui8 index);

  inline bool IsValid() const { return error_.empty(); }
  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

 private:
  Data fds_[2];
  String error_;
};

}  // namespace base
}  // namespace dist_clang
