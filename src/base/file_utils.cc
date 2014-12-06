#include <base/file_utils.h>

#include <base/assert.h>
#include <base/const_string.h>

#include STL(map)
#include STL(regex)

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

namespace dist_clang {
namespace base {

bool CopyFile(const String& src, const String& dst, String* error) {
  struct stat src_stats;
  if (stat(src.c_str(), &src_stats) == -1) {
    GetLastError(error);
    return false;
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

  // Force unlinking of |dst|, since it may be hard-linked with other places.
  if (unlink(dst.c_str()) == -1 && errno != ENOENT) {
    GetLastError(error);
    close(src_fd);
    return false;
  }

  // We need write-access even on object files after introduction of the
  // "split-dwarf" option, see
  // https://sourceware.org/bugzilla/show_bug.cgi?id=971
  auto dst_fd = open(dst.c_str(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC,
                     src_stats.st_mode);
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
      if (bytes_written <= 0) {
        break;
      }
      total += bytes_written;
    }
    if (total < size) {
      break;
    }
  }
  close(src_fd);
  close(dst_fd);

  return !size;
}

bool LinkFile(const String& src, const String& dst, String* error) {
  auto Link = [&src, &dst]() -> int {
#if defined(OS_LINUX)
    // Linux doesn't guarantee that |link()| do dereferences symlinks, thus
    // we use |linkat()| which does for sure.
    return linkat(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(),
                  AT_SYMLINK_FOLLOW);
#elif defined(OS_MACOSX)
    return link(src.c_str(), dst.c_str());
#else
#pragma message "This platform doesn't support hardlinks!"
    errno = EACCES;
    return -1;
#endif
  };

  // Try to create hard-link at first.
  if (Link() == 0 ||
      (errno == EEXIST && unlink(dst.c_str()) == 0 && Link() == 0)) {
    return true;
  }

  return CopyFile(src, dst, error);
}

bool ReadFile(const String& path, Immutable* output, String* error) {
  if (!output) {
    return false;
  }

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

  auto size = FileSize(path);
  auto flags = MAP_PRIVATE;
#if defined(OS_LINUX)
  flags |= MAP_POPULATE;
  void* map = mmap64(nullptr, size, PROT_READ, flags, src_fd, 0);
#elif defined(OS_MACOSX)
  void* map = mmap(nullptr, size, PROT_READ, flags, src_fd, 0);
#else
#pragma message "This platform doesn't support mmap interface!"
  void* map = MAP_FAILED;
#endif
  if (map == MAP_FAILED) {
    GetLastError(error);
    return false;
  }

  output->assign(Immutable(map, size));
  close(src_fd);

  return true;
}

bool WriteFile(const String& path, Immutable input, String* error) {
  // We need write-access even on object files after introduction of the
  // "split-dwarf" option, see
  // https://sourceware.org/bugzilla/show_bug.cgi?id=971
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  auto src_fd = open((path + ".tmp").c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }

  // FIXME: we should respect umask somehow.
  DCHECK([&] {
    struct stat st;
    return fstat(src_fd, &st) == 0 && (st.st_mode & mode) == mode;
  }());

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
    if (size <= 0) {
      break;
    }
    total_bytes += size;
  }
  close(src_fd);

  if (!MoveFile(path + ".tmp", path)) {
    DeleteFile(path + ".tmp");
    return false;
  }

  return total_bytes == input.size();
}

bool WriteFile(const String& path, const String& input, String* error) {
  // We need write-access even on object files after introduction of the
  // "split-dwarf" option, see
  // https://sourceware.org/bugzilla/show_bug.cgi?id=971
  const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  auto src_fd = open((path + ".tmp").c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (src_fd == -1) {
    GetLastError(error);
    return false;
  }

  // FIXME: we should respect umask somehow.
  DCHECK([&] {
    struct stat st;
    return fstat(src_fd, &st) == 0 && (st.st_mode & mode) == mode;
  }());

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
    if (size <= 0) {
      break;
    }
    total_bytes += size;
  }
  close(src_fd);

  if (!MoveFile(path + ".tmp", path)) {
    DeleteFile(path + ".tmp");
    return false;
  }

  return total_bytes == input.size();
}

bool HashFile(const String& path, Immutable* output,
              const List<Literal>& skip_list, String* error) {
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

  output->assign(base::Hexify(output->Hash()));
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

bool CreateDirectory(const String& path, String* error) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '/' && mkdir(path.substr(0, i).c_str(), 0755) == -1 &&
        errno != EEXIST) {
      GetLastError(error);
      return false;
    }
  }

  if (mkdir(path.c_str(), 0755) == -1 && errno != EEXIST) {
    GetLastError(error);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist
