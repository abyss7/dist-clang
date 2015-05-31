#include <base/file_utils.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/string_utils.h>

#include STL(map)
#include STL(regex)

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

void WalkDirectory(
    const String& path,
    Fn<void(const String& file_path, ui64 mtime, ui64 size)> visitor,
    String* error) {
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
              struct timespec time_spec;
#if defined(OS_MACOSX)
              time_spec = buffer.st_mtimespec;
#elif defined(OS_LINUX)
              time_spec = buffer.st_mtim;
#else
#pragma message "Don't know how to get modification time on this platform!"
              NOTREACHED();
#endif
              visitor(new_path, time_spec.tv_sec, buffer.st_size);
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
}

ui64 CalculateDirectorySize(const String& path, String* error) {
  ui64 result = 0u;

  WalkDirectory(path, [&result](const String&, ui64, ui64 size) {
    result += size;
  }, error);

  return result;
}

Pair<time_t> GetModificationTime(const String& path, String* error) {
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
#pragma message "Don't know how to get modification time on this platform!"
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
    auto current_mtime = GetModificationTime(new_path);
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
