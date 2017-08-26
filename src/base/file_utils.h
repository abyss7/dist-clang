#pragma once

#include <base/c_utils.h>

#include <stdio.h>

namespace dist_clang {
namespace base {

void WalkDirectory(
    const Path& path,
    Fn<void(const Path& file_path, ui64 mtime, ui64 size)> visitor,
    String* error = nullptr);
ui64 CalculateDirectorySize(const Path& path, String* error = nullptr);

Pair<time_t /* unix timestamp, nanoseconds */> GetModificationTime(
    const String& path, String* error = nullptr);

// This function returns the path to an object with the least recent
// modification time. It may be regular file, directory or any other kind of
// file. The |path| should be a directory. If there is nothing inside the |path|
// or error has occured, the return value is |false|.
bool GetLeastRecentPath(const Path& path, Path& result,
                        String* error = nullptr);
bool GetLeastRecentPath(const Path& path, Path& result, const char* regex,
                        String* error = nullptr);

inline bool RemoveEmptyDirectory(const String& path) {
  return !rmdir(path.c_str());
}

inline bool ChangeOwner(const String& path, ui32 uid, String* error = nullptr) {
  if (lchown(path.c_str(), uid, getgid()) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

bool CreateDirectory(const String& path, String* error = nullptr);

inline bool ChangeCurrentDir(const Path& path, String* error = nullptr) {
  std::error_code ec;
  std::experimental::filesystem::current_path(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

inline Path GetCurrentDir(String* error = nullptr) {
  std::error_code ec;
  const auto current_dir = std::experimental::filesystem::current_path(ec);
  if (ec && error) {
    *error = ec.message();
  }
  return current_dir;
}

}  // namespace base
}  // namespace dist_clang
