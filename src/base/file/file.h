#pragma once

#include <base/file/data.h>

namespace dist_clang {
namespace base {

class File final : public Data {
 public:
  explicit File(const Path& path);  // Open read-only file

  using Handle::Close;

  inline void GetCreationError(String* error) const {
    if (error) {
      error->assign(error_);
    }
  }

  static bool IsFile(const Path& path, String* error = nullptr);
  static bool IsExecutable(const Path& path, String* error = nullptr);
  static bool Exists(const Path& path, String* error = nullptr);

  ui64 Size(String* error = nullptr) const;
  bool Read(Immutable* output, String* error = nullptr);
  bool Hash(Immutable* output, const List<Literal>& skip_list = List<Literal>(),
            String* error = nullptr);

  bool CopyInto(const Path& dst_path, String* error = nullptr);

  static ui64 Size(const Path& path, String* error = nullptr);
  static bool Read(const Path& path, Immutable* output,
                   String* error = nullptr);
  static bool Write(const Path& path, Immutable input, String* error = nullptr);
  static bool Hash(const Path& path, Immutable* output,
                   const List<Literal>& skip_list = List<Literal>(),
                   String* error = nullptr);

  static bool Copy(const Path& src_path, const Path& dst_path,
                   String* error = nullptr);
  static bool Link(const Path& src_path, const Path& dst_path,
                   String* error = nullptr);
  static bool Move(const Path& src, const Path& dst, String* error = nullptr);

  enum class CloneMode {
    REFLINK,   // A copy-on-write clone - an independent file.
    HARDLINK,  // The same file - may be modified through any of its paths.
    COPY,
  };

  // Makes |dst| to have the contents of |src| in the cheapest way: tries a
  // copy-on-write clone, then a hard link, then a plain copy. The |dst| is
  // replaced atomically, if exists. With |allow_hardlink| = false never shares
  // the file.
  static bool Clone(const Path& src, const Path& dst, bool allow_hardlink,
                    CloneMode* mode = nullptr, String* error = nullptr);
  static bool Delete(const Path& path, String* error = nullptr);

 private:
  File(const Path& path, ui64 size);  // Open truncated write-only file
  bool Close(String* error = nullptr);

  // The parts, that differ between the platforms - in "file_<os>.cc".
  bool AdviseSequentialRead();
  // Only a hint for the file system.
  void Preallocate(ui64 size);
  bool CopyContentsInto(File& dst, ui64 size, String* error);
  // Makes |dst| a copy-on-write clone of |src| - if the file system can. The
  // |dst| shouldn't exist.
  static bool CloneContents(const Path& src, const Path& dst);

  String error_;
  String move_on_close_;
};

}  // namespace base
}  // namespace dist_clang
