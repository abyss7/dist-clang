#pragma once

#include "base/assert.h"
#include "base/c_utils.h"

#include <string>

#include <stdio.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

// This function expects that the source and destination files are immutable -
// i.e. they don't have write permissions.
bool CopyFile(const std::string& src, const std::string& dst,
              bool overwrite = false, std::string* error = nullptr);

inline void DeleteFile(const std::string& path) {
  unlink(path.c_str());
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

}  // namespace base
}  // namespace dist_clang
