#include "daemon/file_cache.h"

#include "base/file_utils.h"
#include "base/hash.h"
#include "base/logging.h"
#include "base/string_utils.h"

#include <fstream>
#include <sys/types.h>
#include <utime.h>

#include "base/using_log.h"

namespace dist_clang {
namespace daemon {

FileCache::FileCache(const std::string &path) : FileCache(path, UNLIMITED) {}

FileCache::FileCache(const std::string &path, uint64_t size)
    : path_(path), max_size_(size), cached_size_(0) {

  if (max_size_ != UNLIMITED) {
    std::string error;
    cached_size_ = base::CalculateDirectorySize(path, &error);
    if (!error.empty()) {
      LOG(CACHE_WARNING)
          << "Error occured during calculation of the cache size: " << error;
    }
  }

  pool_.Run();
}

bool FileCache::Find(const std::string &code, const std::string &command_line,
                     const std::string &version, Entry *entry) const {
  bool result = false;
  const std::string version_hash = base::Hexify(base::MakeHash(version));
  const std::string args_hash = base::Hexify(base::MakeHash(command_line));
  const std::string code_hash = base::Hexify(base::MakeHash(code));

  const std::string version_path = path_ + "/" + version_hash;
  const std::string args_path = version_path + "/" + args_hash;
  const std::string object_path = args_path + "/" + code_hash + "-object";
  const std::string stderr_path = args_path + "/" + code_hash + "-stderr";

  std::ifstream obj(object_path);
  if (obj.is_open()) {
    DCHECK_O_EVAL(utime(object_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(version_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = true;
    entry->first = object_path;
  }

  if (base::FileExists(stderr_path)) {
    DCHECK_O_EVAL(utime(stderr_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(args_path.c_str(), nullptr) == 0);
    DCHECK_O_EVAL(utime(version_path.c_str(), nullptr) == 0);
    if (!entry)
      return true;
    result = base::ReadFile(stderr_path, &entry->second);
  }

  return result;
}

FileCache::Optional FileCache::Store(const std::string &code,
                                     const std::string &command_line,
                                     const std::string &version,
                                     const Entry &entry) {
  std::string version_hash = base::Hexify(base::MakeHash(version));
  std::string args_hash = base::Hexify(base::MakeHash(command_line));
  std::string code_hash = base::Hexify(base::MakeHash(code));
  std::string path = path_ + "/" + version_hash + "/" + args_hash;

  auto task = std::bind(&FileCache::DoStore, this, path, code_hash, entry);
  return pool_.Push(task);
}

void FileCache::SyncStore(const std::string &code,
                          const std::string &command_line,
                          const std::string &version,
                          const Entry &entry) {
  std::string version_hash = base::Hexify(base::MakeHash(version));
  std::string args_hash = base::Hexify(base::MakeHash(command_line));
  std::string code_hash = base::Hexify(base::MakeHash(code));
  std::string path = path_ + "/" + version_hash + "/" + args_hash;

  DoStore(path, code_hash, entry);
}

void FileCache::DoStore(const std::string &path, const std::string &code_hash,
                        const Entry &entry) {
  // FIXME: refactor to portable solution.
  if (system((std::string("mkdir -p ") + path).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << path;
    return;
  }

  const std::string object_path = path + "/" + code_hash + "-object";
  const std::string stderr_path = path + "/" + code_hash + "-stderr";

  if (!base::CopyFile(entry.first, object_path + ".tmp")) {
    LOG(CACHE_ERROR) << "Failed to copy " << entry.first << " to object.tmp";
    return;
  }
  if (!entry.second.empty() && !base::WriteFile(stderr_path, entry.second)) {
    base::DeleteFile(object_path + ".tmp");
    LOG(CACHE_ERROR) << "Failed to write stderr to file";
    return;
  }
  if (!base::MoveFile(object_path + ".tmp", object_path)) {
    base::DeleteFile(object_path + ".tmp");
    base::DeleteFile(stderr_path);
    LOG(CACHE_ERROR) << "Failed to move object.tmp to object";
    return;
  }

  cached_size_ += base::FileSize(object_path) + base::FileSize(stderr_path);
  DCHECK_O_EVAL(utime(object_path.c_str(), nullptr) == 0);
  if (!entry.second.empty()) {
    DCHECK_O_EVAL(utime(stderr_path.c_str(), nullptr) == 0);
  }
  LOG(CACHE_VERBOSE) << "File " << entry.first << " is cached on path " << path;

  if (max_size_ != UNLIMITED) {
    while (cached_size_ > max_size_) {
      std::string version_path, args_path;

      {
        std::string error;
        if (!base::GetLeastRecentPath(path_, version_path, &error)) {
          LOG(CACHE_WARNING) << "Failed to get the recent path from " << path_
                             << " : " << error;
          LOG(CACHE_ERROR) << "Failed to clean the cache";
          break;
        }
      }
      {
        std::string error;
        if (!base::GetLeastRecentPath(version_path, args_path, &error)) {
          if (!base::RemoveDirectory(version_path)) {
            LOG(CACHE_WARNING) << "Failed to get the recent path from "
                               << version_path << " : " << error;
            LOG(CACHE_ERROR) << "Failed to clean the cache";
            break;
          }
          continue;
        }
      }

      bool should_break = false;
      while (cached_size_ > max_size_) {
        std::string file_path;

        {
          std::string error;
          if (!base::GetLeastRecentPath(args_path, file_path, &error)) {
            if (!base::RemoveDirectory(args_path)) {
              LOG(CACHE_WARNING) << "Failed to get the recent path from "
                                 << args_path << " : " << error;
              LOG(CACHE_ERROR) << "Failed to clean the cache";
              should_break = true;
            }
            break;
          }
        }
        {
          std::string error;
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
