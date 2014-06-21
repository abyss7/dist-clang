#include <daemon/file_cache.h>

#include <base/file_utils.h>
#include <base/hash.h>
#include <base/logging.h>
#include <base/string_utils.h>

#include <third_party/libcxx/exported/include/fstream>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

FileCache::FileCache(const String &path) : FileCache(path, UNLIMITED) {}

FileCache::FileCache(const String &path, ui64 size)
    : path_(path), max_size_(size), cached_size_(0) {

  if (max_size_ != UNLIMITED) {
    String error;
    cached_size_ = base::CalculateDirectorySize(path, &error);
    if (!error.empty()) {
      LOG(CACHE_WARNING)
          << "Error occured during calculation of the cache size: " << error;
    }
  }

  pool_.Run();
}

String FileCache::Hash(const String &code, const String &command_line,
                       const String &version) const {
  return base::Hexify(base::MakeHash(code)) + "-" +
         base::Hexify(base::MakeHash(command_line, 4)) + "-" +
         base::Hexify(base::MakeHash(version, 4));
}

bool FileCache::Find(const String &code, const String &command_line,
                     const String &version, Entry *entry) const {
  String &&hash = Hash(code, command_line, version);
  const String first_path = path_ + "/" + hash[0];
  const String second_path = first_path + "/" + hash[1];
  const String common_path = second_path + "/" + hash.substr(2);
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";

  if (!base::FileExists(object_path)) {
    return false;
  }

  utime(object_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  if (entry) {
    entry->object_path = object_path;

    if (base::FileExists(deps_path)) {
      utime(deps_path.c_str(), nullptr);
      utime(second_path.c_str(), nullptr);
      utime(first_path.c_str(), nullptr);

      entry->deps_path = deps_path;
    }

    if (base::FileExists(stderr_path)) {
      utime(stderr_path.c_str(), nullptr);
      utime(second_path.c_str(), nullptr);
      utime(first_path.c_str(), nullptr);
      if (!base::ReadFile(stderr_path, &entry->stderr)) {
        return false;
      }
    }
  }

  return true;
}

FileCache::Optional FileCache::Store(const String &code,
                                     const String &command_line,
                                     const String &version,
                                     const Entry &entry) {
  auto task = std::bind(&FileCache::DoStore, this,
                        Hash(code, command_line, version), entry);
  return pool_.Push(task);
}

void FileCache::StoreNow(const String &code, const String &command_line,
                         const String &version, const Entry &entry) {
  DoStore(Hash(code, command_line, version), entry);
}

void FileCache::DoStore(const String &hash, const Entry &entry) {
  const String first_path = path_ + "/" + hash[0];
  const String second_path = first_path + "/" + hash[1];
  const String common_path = second_path + "/" + hash.substr(2);
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";

  // FIXME: refactor to portable solution.
  if (system((String("mkdir -p ") + second_path).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << second_path;
    return;
  }

  if (!base::CopyFile(entry.object_path, object_path + ".tmp")) {
    LOG(CACHE_ERROR) << "Failed to copy " << entry.object_path << " to "
                     << object_path << ".tmp";
    return;
  }
  if (!entry.deps_path.empty() && !base::CopyFile(entry.deps_path, deps_path)) {
    base::DeleteFile(object_path + ".tmp");
    LOG(CACHE_ERROR) << "Failed to copy " << entry.deps_path << " to "
                     << deps_path;
    return;
  }
  if (!entry.stderr.empty() && !base::WriteFile(stderr_path, entry.stderr)) {
    base::DeleteFile(object_path + ".tmp");
    base::DeleteFile(deps_path);
    LOG(CACHE_ERROR) << "Failed to write stderr to file";
    return;
  }
  if (!base::MoveFile(object_path + ".tmp", object_path)) {
    base::DeleteFile(object_path + ".tmp");
    base::DeleteFile(deps_path);
    base::DeleteFile(stderr_path);
    LOG(CACHE_ERROR) << "Failed to move " << object_path << ".tmp to "
                     << object_path;
    return;
  }

  cached_size_ += base::FileSize(object_path) + base::FileSize(deps_path) +
                  base::FileSize(stderr_path);
  DCHECK_O_EVAL(utime(object_path.c_str(), nullptr) == 0);
  if (!entry.deps_path.empty()) {
    DCHECK_O_EVAL(utime(deps_path.c_str(), nullptr) == 0);
  }
  if (!entry.stderr.empty()) {
    DCHECK_O_EVAL(utime(stderr_path.c_str(), nullptr) == 0);
  }
  LOG(CACHE_VERBOSE) << "File " << entry.object_path << " is cached on path "
                     << object_path;

  if (max_size_ != UNLIMITED) {
    while (cached_size_ > max_size_) {
      String first_path, second_path;

      {
        String error;
        if (!base::GetLeastRecentPath(path_, first_path, &error)) {
          LOG(CACHE_WARNING) << "Failed to get the recent path from " << path_
                             << " : " << error;
          LOG(CACHE_ERROR) << "Failed to clean the cache";
          break;
        }
      }
      {
        String error;
        if (!base::GetLeastRecentPath(first_path, second_path, &error)) {
          if (!base::RemoveDirectory(first_path)) {
            LOG(CACHE_WARNING) << "Failed to get the recent path from "
                               << first_path << " : " << error;
            LOG(CACHE_ERROR) << "Failed to clean the cache";
            break;
          }
          continue;
        }
      }

      bool should_break = false;
      while (cached_size_ > max_size_) {
        String file_path;

        {
          String error;
          if (!base::GetLeastRecentPath(second_path, file_path, ".*\\.o",
                                        &error)) {
            if (!base::RemoveDirectory(second_path)) {
              LOG(CACHE_WARNING) << "Failed to get the recent path from "
                                 << second_path << " : " << error;
              LOG(CACHE_ERROR) << "Failed to clean the cache";
              should_break = true;
            }
            break;
          }
        }
        {
          String error;
          auto file_size = base::FileSize(file_path);
          if (!base::DeleteFile(file_path, &error)) {
            LOG(CACHE_WARNING) << "Failed to delete file " << file_path << " : "
                               << error;
            LOG(CACHE_ERROR) << "Failed to clean the cache";
            should_break = true;
            break;
          }
          CHECK(file_size <= cached_size_);
          cached_size_ -= file_size;
        }
      }

      if (should_break) {
        break;
      }
    }
  }
}

}  // namespace daemon
}  // namespace dist_clang
