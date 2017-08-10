#include <base/file_utils.h>

#include <base/c_utils.h>
#include <base/const_string.h>

#include STL(chrono)
#include STL_EXPERIMENTAL(filesystem)
#include STL(map)
#include STL(regex)
#include STL(system_error)

namespace dist_clang {
namespace base {

void WalkDirectory(
    const Path& path,
    Fn<void(const Path& file_path, ui64 mtime, ui64 size)> visitor,
    String* error) {
  namespace fs = std::experimental::filesystem;
  auto has_error = [&error](const std::error_code& ec) {
    if (ec) {
      if (error) {
        *error = ec.message();
      }
      return true;
    }
    return false;
  };
  std::error_code ec;
  // TODO(matthewtff): Consider following symlinks & skip permission denied
  // directories.
  fs::recursive_directory_iterator directories(path, ec);

  if (has_error(ec)) {
    return;
  }

  for (const auto& directory_entry : directories) {
    // TODO(matthewtff): Reimplement using directory_entry::is_regular_file()
    // as it gets implemented in libcxx.
    const auto& entry_status = directory_entry.status(ec);
    if (has_error(ec)) {
      return;
    }

    if (!fs::is_regular_file(entry_status)) {
      continue;
    }

    const auto& path = directory_entry.path();

    const auto& last_write_time = fs::last_write_time(path, ec);
    if (has_error(ec)) {
      return;
    }

    const auto& file_size = fs::file_size(path, ec);
    if (has_error(ec)) {
      return;
    }
    const auto seconds_since_epoch = std::chrono::duration_cast<Seconds>(
        last_write_time.time_since_epoch()).count();
    visitor(path, seconds_since_epoch, file_size);
  }
}

ui64 CalculateDirectorySize(const Path& path, String* error) {
  ui64 result = 0u;

  WalkDirectory(path, [&result](const Path&, ui64, ui64 size) {
    result += size;
  }, error);

  return result;
}

bool RemoveEmptyDirectory(const Path& path) {
  std::error_code ec;
  return std::experimental::filesystem::remove(path, ec);
}

bool ChangeOwner(const Path& path, ui32 uid, String* error) {
#if defined(OS_WIN)
#error NOTIMPLEMENTED
#else
  if (lchown(path.c_str(), uid, getgid()) == -1) {
    GetLastError(error);
    return false;
  }
  return true;
#endif
}

bool CreateDirectory(const Path& path, String* error) {
#if defined(OS_WIN)
  std::error_code ec;
  std::experimental::filesystem::create_directories(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
#else
  const String& path_str = path.string();
  for (size_t i = 1; i < path_str.size(); ++i) {
    if (path_str[i] == '/' &&
        mkdir(path_str.substr(0, i).c_str(), 0755) == -1 &&
        errno != EEXIST) {
      GetLastError(error);
      return false;
    }
  }

  if (mkdir(path_str.c_str(), 0755) == -1 && errno != EEXIST) {
    GetLastError(error);
    return false;
  }
#endif
  return true;
}

Path AppendExtension(Path path, const char* extension) {
  return path += extension;
}

}  // namespace base
}  // namespace dist
