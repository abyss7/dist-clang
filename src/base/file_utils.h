#pragma once

#include "base/c_utils.h"

#include <string>

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

void DeleteFile(const std::string& path);

inline bool CopyFile(const std::string& src, const std::string& dst,
                     bool overwrite = false, std::string* error = nullptr) {
  auto src_fd = open(src.c_str(), O_RDONLY);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }
  struct stat src_stats;
  if (fstat(src_fd, &src_stats) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  auto flags = O_CREAT|O_WRONLY;
  if (!overwrite)
    flags |= O_EXCL;
  else
    flags |= O_TRUNC;
  auto dst_fd = open(dst.c_str(), flags, src_stats.st_mode & 0777);
  if (dst_fd == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  const size_t buffer_size = 1024;
  char buffer[buffer_size];
  int size = 0;
  while((size = read(src_fd, buffer, buffer_size)) > 0) {
    int total = 0;
    int bytes_written = 0;
    while(total < size) {
      bytes_written = write(dst_fd, buffer + total, size - total);
      if (bytes_written <= 0)
        break;
      total += bytes_written;
    }
    if (total < size)
      break;
  }
  close(src_fd);
  close(dst_fd);

  return !size;
}

inline void DeleteFile(const std::string& path) {
  unlink(path.c_str());
}

inline bool MoveFile(const std::string& src, const std::string& dst) {
  return rename(src.c_str(), dst.c_str()) != -1;
}

inline bool ReadFile(const std::string& path, std::string* output,
                     std::string* error = nullptr) {
  if (!output)
    return false;
  output->clear();

  auto src_fd = open(path.c_str(), O_RDONLY);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }

  const size_t buffer_size = 1024;
  char buffer[buffer_size];
  int size = 0;
  while((size = read(src_fd, buffer, buffer_size)) > 0)
    output->append(std::string(buffer, size));
  close(src_fd);

  return !size;
}

inline bool WriteFile(const std::string& path, const std::string& input,
                      std::string* error = nullptr) {
  auto src_fd = open(path.c_str(), O_WRONLY|O_TRUNC|O_CREAT);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }

  int total_bytes = 0;
  int size = 0;
  while(total_bytes < input.size()) {
    size = write(src_fd, input.data() + total_bytes,
                 input.size() - total_bytes);
    if (size <= 0)
      break;
    total_bytes += size;
  }
  close(src_fd);

  return total_bytes == input.size();
}

}  // namespace base
}  // namespace dist_clang
