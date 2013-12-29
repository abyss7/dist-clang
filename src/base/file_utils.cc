#include "base/file_utils.h"

#include <fcntl.h>
#include <sys/stat.h>

namespace dist_clang {
namespace base {

bool CopyFile(const std::string& src, const std::string& dst, bool overwrite,
              std::string* error) {
  // TODO: check, that the source file is immutable.

  // Try to create hard-link at first.
  if (link(src.c_str(), dst.c_str()) == 0) {
    // TODO: remove 'write' permissions from the destination file.
    return true;
  }
  else if (errno == EEXIST && overwrite &&
           unlink(dst.c_str()) == 0 && link(src.c_str(), dst.c_str()) == 0) {
    // TODO: remove 'write' permissions from the destination file.
    return true;
  }

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
  if (posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  auto flags = O_CREAT|O_WRONLY|O_EXCL;
  // Force unlinking of |dst|, since it may be hard-linked with other places.
  if (overwrite && unlink(dst.c_str()) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }
  auto dst_fd = open(dst.c_str(), flags, src_stats.st_mode & 0777);
  if (dst_fd == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }
  // FIXME: may be, we should allocate st_blocks*st_blk_size?
  if (posix_fallocate(dst_fd, 0, src_stats.st_size) == -1) {
    GetLastError(error);
    close(src_fd);
    close(dst_fd);
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

  // TODO: make the destination file immutable.

  return !size;
}

bool ReadFile(const std::string& path, std::string* output,
              std::string* error) {
  if (!output)
    return false;
  output->clear();

  auto src_fd = open(path.c_str(), O_RDONLY);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }
  if (posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(error);
    close(src_fd);
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

bool WriteFile(const std::string& path, const std::string& input,
               std::string* error) {
  auto src_fd = open((path + ".tmp").c_str(), O_WRONLY|O_TRUNC|O_CREAT, 0444);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }
  if (posix_fallocate(src_fd, 0, input.size()) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  size_t total_bytes = 0;
  int size = 0;
  while(total_bytes < input.size()) {
    size = write(src_fd, input.data() + total_bytes,
                 input.size() - total_bytes);
    if (size <= 0)
      break;
    total_bytes += size;
  }
  close(src_fd);

  if (!MoveFile(path + ".tmp", path)) {
    DeleteFile(path + ".tmp");
    return false;
  }

  return total_bytes == input.size();
}

}  // namespace base
}  // namespace dist
