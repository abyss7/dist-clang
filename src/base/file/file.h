#pragma once

#include <base/file/data.h>

namespace dist_clang {
namespace base {

class File final : public Data {
 public:
  explicit File(const String& path);  // Open read-only file

  using Handle::Close;

  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

  static bool IsFile(const String& path, String* error = nullptr);
  static bool IsExecutable(const String& path, String* error = nullptr);
  static bool Exists(const String& path, String* error = nullptr);

  ui64 Size(String* error = nullptr) const;
  bool Read(Immutable* output, String* error = nullptr);
  bool Hash(Immutable* output, const List<Literal>& skip_list = List<Literal>(),
            String* error = nullptr);

  bool CopyInto(const String& dst_path, String* error = nullptr);

  static ui64 Size(const String& path, String* error = nullptr);
  static bool Read(const String& path, Immutable* output,
                   String* error = nullptr);
  static bool Write(const String& path, Immutable input,
                    String* error = nullptr);
  static bool Hash(const String& path, Immutable* output,
                   const List<Literal>& skip_list = List<Literal>(),
                   String* error = nullptr);

  static bool Copy(const String& src_path, const String& dst_path,
                   String* error = nullptr);
  static bool Link(const String& src_path, const String& dst_path,
                   String* error = nullptr);
  static bool Move(const String& src, const String& dst,
                   String* error = nullptr);
  static bool Delete(const String& path, String* error = nullptr);

  static String TmpUniqFile();

 private:
  File(const String& path, ui64 size);  // Open truncated write-only file
  bool Close(String* error = nullptr);

  String error_;
  String move_on_close_;
};

}  // namespace base
}  // namespace dist_clang
