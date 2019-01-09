#pragma once

#include <base/c_utils.h>

#include <stdio.h>

namespace dist_clang {
namespace base {

// TODO: refactor on |std::filesystem|.
void WalkDirectory(
    const Path& path,
    Fn<void(const Path& file_path, ui64 mtime, ui64 size)> visitor,
    String* error = nullptr);

// TODO: refactor on |std::filesystem|.
ui64 CalculateDirectorySize(const Path& path, String* error = nullptr);

// TODO: refactor on |std::filesystem| and |std::chrono|.
Pair<time_t /* unix timestamp, nanoseconds */> GetModificationTime(
    const String& path, String* error = nullptr);

// TODO: refactor on |std::filesystem|.
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

inline bool CreateDirectory(const Path& path, String* error = nullptr) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

inline bool ChangeCurrentDir(const Path& path, String* error = nullptr) {
  std::error_code ec;
  std::filesystem::current_path(path, ec);
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
  const auto current_dir = std::filesystem::current_path(ec);
  if (ec && error) {
    *error = ec.message();
  }
  return current_dir;
}

}  // namespace base
}  // namespace dist_clang
