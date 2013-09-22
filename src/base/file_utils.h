#pragma once

#include <string>

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

void DeleteFile(const std::string& path);

inline bool CopyFile(const std::string& src, const std::string& dst,
                     bool overwrite = false) {
  auto src_fd = open(src.c_str(), O_RDONLY);
  if (src_fd == -1)
    return false;
  struct stat src_stats;
  if (fstat(src_fd, &src_stats) == -1) {
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
    close(src_fd);
    return false;
  }

  char buf[1024];
  int size = 0;
  while((size = read(src_fd, buf, 1024)) > 0)
    write(dst_fd, buf, size);
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

}  // namespace base
}  // namespace dist_clang
