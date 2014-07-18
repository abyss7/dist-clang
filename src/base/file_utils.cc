#include <base/file_utils.h>

#include <base/hash.h>

#include <third_party/libcxx/exported/include/map>
#include <third_party/libcxx/exported/include/regex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

namespace dist_clang {

namespace {

int Link(const char* src, const char* dst) {
#if defined(OS_LINUX)
  // Linux doesn't guarantee that |link()| do dereferences symlinks, thus
  // we use |linkat()| which does for sure.
  return linkat(AT_FDCWD, src, AT_FDCWD, dst, AT_SYMLINK_FOLLOW);
#elif defined(OS_MACOSX)
  return link(src, dst);
#else
#pragma message "This platform doesn't support hardlinks!"
  errno = EACCES;
  return -1;
#endif
}

}  // namespace

namespace base {

bool CopyFile(const String& src, const String& dst, bool overwrite,
              bool no_hardlink, String* error) {
  struct stat src_stats;
  if (stat(src.c_str(), &src_stats) == -1) {
    GetLastError(error);
    return false;
  }

  if (!no_hardlink) {
    // Try to create hard-link at first.
    if (Link(src.c_str(), dst.c_str()) == 0) {
      return true;
    } else if (errno == EEXIST && overwrite && unlink(dst.c_str()) == 0 &&
               Link(src.c_str(), dst.c_str()) == 0) {
      return true;
    }
  }

  auto src_fd = open(src.c_str(), O_RDONLY);
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

  auto flags = O_CREAT | O_WRONLY | O_EXCL;
  // Force unlinking of |dst|, since it may be hard-linked with other places.
  if (overwrite && unlink(dst.c_str()) == -1 && errno != ENOENT) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  // We need write-access even on object files after introduction of the
  // "split-dwarf" option, see
  // https://sourceware.org/bugzilla/show_bug.cgi?id=971
  auto dst_fd = open(dst.c_str(), flags, src_stats.st_mode);
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

  const size_t buffer_size = 8192;
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

  return !size;
}

bool ReadFile(const String& path, String* output, String* error) {
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
    output->append(String(buffer, size));
  }
  if (size == -1) {
    GetLastError(error);
  }
  close(src_fd);

  return !size;
}

bool WriteFile(const String& path, const String& input, String* error) {
  // We need write-access even on object files after introduction of the
  // "split-dwarf" option, see
  // https://sourceware.org/bugzilla/show_bug.cgi?id=971
  auto src_fd =
      open((path + ".tmp").c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0664);
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

bool HashFile(const String& path, String* output,
              const List<const char*>& skip_list, String* error) {
  if (!ReadFile(path, output, error)) {
    return false;
  }

  for (const char* skip : skip_list) {
    if (output->find(skip) != String::npos) {
      if (error) {
        error->assign("Skip-list hit: " + String(skip));
      }
      return false;
    }
  }

  output->assign(base::Hexify(MakeHash(*output)));
  return true;
}

ui64 CalculateDirectorySize(const String& path, String* error) {
  ui64 size = 0u;
  List<String> paths;
  paths.push_back(path);

  while (!paths.empty()) {
    const String& path = paths.front();
    if (DIR* dir = opendir(path.c_str())) {
      struct dirent* entry = nullptr;

      while ((entry = readdir(dir))) {
        const String entry_name = entry->d_name;
        const String new_path = path + "/" + entry_name;

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
            break;
          }
        }
      }
      closedir(dir);
    } else {
      GetLastError(error);
      break;
    }

    paths.pop_front();
  }

  return size;
}

Pair<time_t> GetLastModificationTime(const String& path, String* error) {
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

bool GetLeastRecentPath(const String& path, String& result, const char* regex,
                        String* error) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    GetLastError(error);
    return false;
  }

  const Pair<time_t> null_time = {0, 0};
  Pair<time_t> mtime = null_time;
  while (true) {
    const struct dirent* entry = readdir(dir);
    if (!entry) {
      break;
    }

    if (!std::regex_match(entry->d_name, std::regex(regex))) {
      continue;
    }

    const String entry_name = entry->d_name;
    if (entry_name == "." || entry_name == "..") {
      continue;
    }

    const String new_path = path + "/" + entry_name;
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

bool GetLeastRecentPath(const String& path, String& result, String* error) {
  return GetLeastRecentPath(path, result, ".*", error);
}

}  // namespace base
}  // namespace dist
