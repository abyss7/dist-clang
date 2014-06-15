#pragma once

#include <base/assert.h>
#include <base/c_utils.h>

#include <string>

#include <stdio.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

// This function expects that the source and destination files are immutable -
// i.e. they don't have write permissions.
bool CopyFile(const std::string& src, const std::string& dst,
              bool overwrite = false, std::string* error = nullptr);

inline bool DeleteFile(const std::string& path, std::string* error = nullptr) {
  if (unlink(path.c_str())) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool FileExists(const std::string& path, std::string* error = nullptr) {
  if (access(path.c_str(), F_OK)) {
    GetLastError(error);
    return false;
  }
  return true;
}

inline bool MoveFile(const std::string& src, const std::string& dst) {
  return rename(src.c_str(), dst.c_str()) != -1;
}

bool ReadFile(const std::string& path, std::string* output,
              std::string* error = nullptr);

bool WriteFile(const std::string& path, const std::string& input,
               std::string* error = nullptr);

ui64 CalculateDirectorySize(const std::string& path,
                            std::string* error = nullptr);

inline ui64 FileSize(const std::string& path, std::string* error = nullptr) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer)) {
    GetLastError(error);
    return 0;
  }

  return buffer.st_size;
}

std::pair<time_t /* unix timestamp */, time_t /* nanoseconds */>
    GetLastModificationTime(const std::string& path,
                            std::string* error = nullptr);

// This function returns the path to an object with the least recent
// modification time. It may be file, directory or any other kind of file.
// The |path| should be a directory. If there is nothing inside the |path| or
// error has occured, the return value is |false|.
bool GetLeastRecentPath(const std::string& path, std::string& result,
                        std::string* error = nullptr);

inline bool RemoveDirectory(const std::string& path) {
  return !rmdir(path.c_str());
}

inline bool ChangeOwner(const std::string& path, ui32 uid,
                        std::string* error = nullptr) {
  if (lchown(path.c_str(), uid, getgid()) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
}

}  // namespace base
}  // namespace dist_clang
