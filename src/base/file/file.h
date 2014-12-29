#pragma once

#include <base/file/data.h>

namespace dist_clang {
namespace base {

class File final : public Data {
 public:
  explicit File(const String& path);  // Open read-only file

  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

  ui64 Size(String* error = nullptr) const;
  bool Read(Immutable* output, String* error = nullptr);
  bool Hash(Immutable* output, const List<Literal>& skip_list = List<Literal>(),
            String* error = nullptr);

  static bool Read(const String& path, Immutable* output,
                   String* error = nullptr);
  static bool Hash(const String& path, Immutable* output,
                   const List<Literal>& skip_list = List<Literal>(),
                   String* error = nullptr);

 private:
  String error_;
};

}  // namespace base
}  // namespace dist_clang
