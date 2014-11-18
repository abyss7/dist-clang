#pragma once

#include <base/c_utils.h>

#include <stdio.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

bool CopyFile(const String& src, const String& dst, String* error = nullptr);

bool LinkFile(const String& src, const String& dst, String* error = nullptr);

inline bool DeleteFile(const String& path, String* error = nullptr) {
  if (unlink(path.c_str())) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool FileExists(const String& path, String* error = nullptr) {
  if (access(path.c_str(), F_OK)) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool IsExecutable(const String& path, String* error = nullptr) {
  if (access(path.c_str(), X_OK)) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool MoveFile(const String& src, const String& dst) {
  return rename(src.c_str(), dst.c_str()) != -1;
}

bool ReadFile(const String& file, Immutable* output, String* error = nullptr);

bool WriteFile(const String& path, Immutable input, String* error = nullptr);
bool WriteFile(const String& path, const String& input,
               String* error = nullptr);

bool HashFile(const String& file, Immutable* output,
              const List<Literal>& skip_list = List<Literal>(),
              String* error = nullptr);

ui64 CalculateDirectorySize(const String& path, String* error = nullptr);

inline ui64 FileSize(const String& path, String* error = nullptr) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer)) {
    GetLastError(error);
    return 0;
  }

  return buffer.st_size;
}

Pair<time_t /* unix timestamp, nanoseconds */> GetLastModificationTime(
    const String& path, String* error = nullptr);

// This function returns the path to an object with the least recent
// modification time. It may be file, directory or any other kind of file.
// The |path| should be a directory. If there is nothing inside the |path| or
// error has occured, the return value is |false|.
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
