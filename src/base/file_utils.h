#pragma once

#include <base/c_utils.h>

#include <stdio.h>

namespace dist_clang {
namespace base {

ui64 CalculateDirectorySize(const String& path, String* error = nullptr);

Pair<time_t /* unix timestamp, nanoseconds */> GetLastModificationTime(
    const String& path, String* error = nullptr);

// This function returns the path to an object with the least recent
// modification time. It may be regular file, directory or any other kind of
// file. The |path| should be a directory. If there is nothing inside the |path|
// or error has occured, the return value is |false|.
bool GetLeastRecentPath(const String& path, String& result,
                        String* error = nullptr);
bool GetLeastRecentPath(const String& path, String& result, const char* regex,
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

}  // namespace base
}  // namespace dist_clang
