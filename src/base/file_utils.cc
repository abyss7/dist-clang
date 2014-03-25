#include "base/file_utils.h"

#include <list>
#include <map>

#include <dirent.h>
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
  } else if (errno == EEXIST && overwrite && unlink(dst.c_str()) == 0 &&
             link(src.c_str(), dst.c_str()) == 0) {
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
#if defined(OS_LINUX)
  if (posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }
#endif  // defined(OS_LINUX)

  auto flags = O_CREAT | O_WRONLY | O_EXCL;
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
#if defined(OS_LINUX)
  // FIXME: may be, we should allocate st_blocks*st_blk_size?
  if (posix_fallocate(dst_fd, 0, src_stats.st_size) == -1) {
    GetLastError(error);
    close(src_fd);
    close(dst_fd);
    return false;
  }
#endif  // defined(OS_LINUX)

  const size_t buffer_size = 1024;
  char buffer[buffer_size];
  int size = 0;
  while ((size = read(src_fd, buffer, buffer_size)) > 0) {
    int total = 0;
    int bytes_written = 0;
    while (total < size) {
      bytes_written = write(dst_fd, buffer + total, size - total);
      if (bytes_written <= 0) break;
      total += bytes_written;
    }
    if (total < size) break;
  }
  close(src_fd);
  close(dst_fd);

  // TODO: make the destination file immutable.

  return !size;
}

bool ReadFile(const std::string& path, std::string* output,
              std::string* error) {
  if (!output) {
    return false;
  }
  output->clear();

  auto src_fd = open(path.c_str(), O_RDONLY);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }
#if defined(OS_LINUX)
  if (posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }
#endif  // defined(OS_LINUX)

  const size_t buffer_size = 1024;
  char buffer[buffer_size];
  int size = 0;
  while ((size = read(src_fd, buffer, buffer_size)) > 0) {
    output->append(std::string(buffer, size));
  }
  if (size == -1) {
    GetLastError(error);
  }
  close(src_fd);

  return !size;
}

bool WriteFile(const std::string& path, const std::string& input,
               std::string* error) {
  auto src_fd =
      open((path + ".tmp").c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0444);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }
#if defined(OS_LINUX)
  if (posix_fallocate(src_fd, 0, input.size()) == -1) {
    GetLastError(error);
    close(src_fd);
    return false;
  }
#endif  // defined(OS_LINUX)

  size_t total_bytes = 0;
  int size = 0;
  while (total_bytes < input.size()) {
    size =
        write(src_fd, input.data() + total_bytes, input.size() - total_bytes);
    if (size <= 0) break;
    total_bytes += size;
  }
  close(src_fd);

  if (!MoveFile(path + ".tmp", path)) {
    DeleteFile(path + ".tmp");
    return false;
  }

  return total_bytes == input.size();
}

uint64_t CalculateDirectorySize(const std::string& path, std::string* error) {
  uint64_t size = 0u;
  std::list<std::string> paths;
  paths.push_back(path);

  while (!paths.empty()) {
    const std::string& path = paths.front();
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
      GetLastError(error);
    } else {
      struct dirent* entry = nullptr;

      while ((entry = readdir(dir))) {
        const std::string entry_name = entry->d_name;
        const std::string new_path = path + "/" + entry_name;

        if (entry_name != "." && entry_name != "..") {
          struct stat buffer;
          if (!stat(new_path.c_str(), &buffer)) {
            if (S_ISDIR(buffer.st_mode)) {
              paths.push_back(new_path);
            } else if (S_ISREG(buffer.st_mode)) {
              size += buffer.st_size;
            }
          } else {
            GetLastError(error);
          }
        }
      }
    }
    closedir(dir);

    paths.pop_front();
  }

  return size;
}

std::pair<time_t, time_t> GetLastModificationTime(const std::string& path,
                                                  std::string* error) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer) == -1) {
    GetLastError(error);
    return {0, 0};
  }

  struct timespec time_spec;
#if defined(OS_MACOSX)
  time_spec = buffer.st_mtimespec;
#elif defined(OS_LINUX)
  time_spec = buffer.st_mtim;
#else
  NOTREACHED();
#endif
  return {time_spec.tv_sec, time_spec.tv_nsec};
}

bool GetLeastRecentPath(const std::string& path, std::string& result,
                        std::string* error) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    GetLastError(error);
    return false;
  }

  const std::pair<time_t, time_t> null_time = {0, 0};
  std::pair<time_t, time_t> mtime = null_time;
  while (true) {
    const struct dirent* entry = readdir(dir);
    if (!entry) {
      break;
    }

    const std::string entry_name = entry->d_name;
    if (entry_name == "." || entry_name == "..") {
      continue;
    }

    const std::string new_path = path + "/" + entry_name;
    auto current_mtime = GetLastModificationTime(new_path);
    if (mtime == null_time ||
        (current_mtime != null_time && current_mtime < mtime)) {
      mtime = current_mtime;
      result = new_path;
    }
  }
  closedir(dir);

  return mtime != null_time;
}

}  // namespace base
}  // namespace dist
