#include <base/file_utils.h>

#include <base/assert.h>
#include <base/const_string.h>
#include <base/platform.h>
#include <base/string_utils.h>

#include STL(map)
#include STL(regex)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

String FileVersion(const String& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == -1) {
    return String();
  }
  // Not the ctime: it changes with the status too - e.g. with a new link to
  // the file, that "perf" makes in its cache.
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(st.st_size) + ":" +
         std::to_string(ModificationTime(st));
}

void WalkDirectory(
    const Path& path,
    Fn<void(const Path& file_path, ui64 mtime, ui64 size)> visitor,
    String* error) {
  List<Path> paths;
  paths.push_back(path);

  while (!paths.empty()) {
    const Path& path = paths.front();
    if (DIR* dir = opendir(path.c_str())) {
      struct dirent* entry = nullptr;

      while ((entry = readdir(dir))) {
        const String entry_name = entry->d_name;
        const Path new_path = path / entry_name;

        if (entry_name != "." && entry_name != "..") {
          // The symlinks aren't followed - out of the walked directory.
          struct stat buffer;
          if (!lstat(new_path.c_str(), &buffer)) {
            if (S_ISDIR(buffer.st_mode)) {
              paths.push_back(new_path);
            } else if (S_ISREG(buffer.st_mode)) {
              visitor(new_path, ModificationTime(buffer) / 1'000'000'000,
                      buffer.st_size);
            }
          } else if (errno != ENOENT) {
            // The vanished files are skipped silently, the others - with the
            // error: the rest of the directory is walked anyway.
            GetLastError(error);
          }
        }
      }
      closedir(dir);
    } else if (errno != ENOENT) {
      GetLastError(error);
    }

    paths.pop_front();
  }
}

ui64 CalculateDirectorySize(const Path& path, String* error) {
  ui64 result = 0u;

  WalkDirectory(
      path, [&result](const Path&, ui64, ui64 size) { result += size; }, error);

  return result;
}

Pair<time_t> GetModificationTime(const String& path, String* error) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer) == -1) {
    GetLastError(error);
    return {0, 0};
  }

  const i64 time = ModificationTime(buffer);
  return {time / 1'000'000'000, time % 1'000'000'000};
}

}  // namespace base
}  // namespace dist_clang
