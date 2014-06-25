#include <file_cache/file_cache.h>

#include <base/file_utils.h>
#include <base/hash.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <file_cache/manifest.pb.h>
#include <file_cache/manifest_utils.h>

#include <third_party/libcxx/exported/include/fstream>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

FileCache::FileCache(const String &path, ui64 size)
    : path_(path),
      pool_(base::ThreadPool::TaskQueue::UNLIMITED, 1),
      max_size_(size),
      cached_size_(0) {
  if (max_size_ != UNLIMITED) {
    String error;

    // FIXME: refactor to portable solution.
    system((String("mkdir -p ") + path).c_str());

    cached_size_ = base::CalculateDirectorySize(path, &error);
    if (!error.empty()) {
      max_size_ = UNLIMITED;
      LOG(CACHE_WARNING)
          << "Error occured during calculation of the cache size: " << error;
    }
  }

  pool_.Run();
}

FileCache::FileCache(const String &path) : FileCache(path, UNLIMITED) {}

// static
String FileCache::Hash(const String &code, const String &command_line,
                       const String &version) {
  return base::Hexify(base::MakeHash(code)) + "-" +
         base::Hexify(base::MakeHash(command_line, 4)) + "-" +
         base::Hexify(base::MakeHash(version, 4));
}

bool FileCache::Find(const String &code, const String &command_line,
                     const String &version, Entry *entry) const {
  const String hash = Hash(code, command_line, version);
  const String first_path = FirstPath(hash);
  const String second_path = SecondPath(hash);
  const String manifest_path = CommonPath(hash) + ".manifest";

  if (!LockForReading(manifest_path)) {
    return false;
  }

  proto::Manifest manifest;
  if (!file_cache::LoadManifest(manifest_path, &manifest)) {
    UnlockForReading(manifest_path);
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  if (entry) {
    if (manifest.stderr()) {
      const String stderr_path = CommonPath(hash) + ".stderr";
      if (!base::ReadFile(stderr_path, &entry->stderr)) {
        UnlockForReading(manifest_path);
        return false;
      }
    }

    if (manifest.object()) {
      entry->object_path = CommonPath(hash) + ".o";
      if (!base::FileExists(entry->object_path)) {
        UnlockForReading(manifest_path);
        return false;
      }
    }

    if (manifest.deps()) {
      entry->deps_path = CommonPath(hash) + ".d";
      if (!base::FileExists(entry->deps_path)) {
        UnlockForReading(manifest_path);
        return false;
      }
    }
  }

  UnlockForReading(manifest_path);
  return true;
}

FileCache::Optional FileCache::Store(const String &code,
                                     const String &command_line,
                                     const String &version,
                                     const Entry &entry) {
  auto &&hash = Hash(code, command_line, version);
  auto task = std::bind(&FileCache::DoStore, this, hash, entry);
  return pool_.Push(task);
}

void FileCache::StoreNow(const String &code, const String &command_line,
                         const String &version, const Entry &entry) {
  DoStore(Hash(code, command_line, version), entry);
}

bool FileCache::RemoveEntry(const String &manifest_path) {
  const String common_path =
      manifest_path.substr(0, manifest_path.size() - sizeof(".manifest") + 1);
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
  bool result = true;

  if (base::FileExists(object_path)) {
    auto size = base::FileSize(object_path);
    if (!base::DeleteFile(object_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  if (base::FileExists(deps_path)) {
    auto size = base::FileSize(deps_path);
    if (!base::DeleteFile(deps_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  if (base::FileExists(stderr_path)) {
    auto size = base::FileSize(stderr_path);
    if (!base::DeleteFile(stderr_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  DCHECK(base::FileExists(manifest_path));
  auto size = base::FileSize(manifest_path);
  if (!base::DeleteFile(manifest_path)) {
    result = false;
  } else {
    DCHECK(size <= cached_size_);
    cached_size_ -= size;
  }

  return result;
}

void FileCache::DoStore(const String &hash, const Entry &entry) {
  const String manifest_path = CommonPath(hash) + ".manifest";

  if (!LockForWriting(manifest_path)) {
    return;
  }

  // FIXME: refactor to portable solution.
  if (system((String("mkdir -p ") + SecondPath(hash)).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    UnlockForWriting(manifest_path);
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << SecondPath(hash);
    return;
  }

  proto::Manifest manifest;
  if (!entry.stderr.empty()) {
    const String stderr_path = CommonPath(hash) + ".stderr";
    if (!base::WriteFile(stderr_path, entry.stderr)) {
      RemoveEntry(manifest_path);
      UnlockForWriting(manifest_path);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path;
      return;
    }
    manifest.set_stderr(true);
    cached_size_ += base::FileSize(stderr_path);
  }

  if (!entry.object_path.empty()) {
    const String object_path = CommonPath(hash) + ".o";
    String error;
    bool result = base::CopyFile(entry.object_path, object_path, true, &error);
    if (entry.move_object) {
      base::DeleteFile(entry.object_path);
    }
    if (!result) {
      RemoveEntry(manifest_path);
      UnlockForWriting(manifest_path);
      LOG(CACHE_ERROR) << "Failed to copy " << entry.object_path
                       << " with error: " << error;
      return;
    }
    cached_size_ += base::FileSize(object_path);
  } else {
    manifest.set_object(false);
  }

  if (!entry.deps_path.empty()) {
    const String deps_path = CommonPath(hash) + ".d";
    String error;
    bool result = base::CopyFile(entry.deps_path, deps_path, true, &error);
    if (entry.move_deps) {
      base::DeleteFile(entry.deps_path);
    }
    if (!result) {
      RemoveEntry(manifest_path);
      UnlockForWriting(manifest_path);
      LOG(CACHE_ERROR) << "Failed to copy " << entry.deps_path
                       << " with error: " << error;
      return;
    }
    cached_size_ += base::FileSize(deps_path);
  } else {
    manifest.set_deps(false);
  }

  if (!file_cache::SaveManifest(manifest_path, manifest)) {
    RemoveEntry(manifest_path);
    UnlockForWriting(manifest_path);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path;
    return;
  }
  cached_size_ += base::FileSize(manifest_path);
  DCHECK_O_EVAL(utime(manifest_path.c_str(), nullptr) == 0);
  DCHECK_O_EVAL(utime(SecondPath(hash).c_str(), nullptr) == 0);
  DCHECK_O_EVAL(utime(FirstPath(hash).c_str(), nullptr) == 0);

  LOG(CACHE_VERBOSE) << "File " << entry.object_path << " is cached on path "
                     << CommonPath(hash);

  UnlockForWriting(manifest_path);
  Clean();
}

void FileCache::Clean() {
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
          if (!base::RemoveEmptyDirectory(first_path)) {
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
        String manifest_path;
        String error;
        if (!base::GetLeastRecentPath(second_path, manifest_path,
                                      ".*\\.manifest", &error)) {
          if (!base::RemoveEmptyDirectory(second_path)) {
            LOG(CACHE_WARNING) << "Failed to get the recent path from "
                               << second_path << " : " << error;
            LOG(CACHE_ERROR) << "Failed to clean the cache";
            should_break = true;
          }
          break;
        }

        if (LockForWriting(manifest_path)) {
          should_break = !RemoveEntry(manifest_path);
          UnlockForWriting(manifest_path);
        }
      }

      if (should_break) {
        break;
      }
    }
  }
}

bool FileCache::LockForReading(const String &path) const {
  if (!base::FileExists(path)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(locks_mutex_);

  if (write_locks_.find(path) != write_locks_.end()) {
    return false;
  }

  auto it = read_locks_.find(path);
  if (it == read_locks_.end()) {
    it = read_locks_.emplace(path, 0).first;
  }
  it->second++;

  return true;
}

void FileCache::UnlockForReading(const String &path) const {
  std::lock_guard<std::mutex> lock(locks_mutex_);

  auto it = read_locks_.find(path);
  CHECK(it != read_locks_.end() && it->second > 0);
  it->second--;

  if (it->second == 0) {
    read_locks_.erase(it);
  }
}

bool FileCache::LockForWriting(const String &path) const {
  std::lock_guard<std::mutex> lock(locks_mutex_);

  if (write_locks_.find(path) != write_locks_.end() ||
      read_locks_.find(path) != read_locks_.end()) {
    return false;
  }

  write_locks_.insert(path);
  return true;
}

void FileCache::UnlockForWriting(const String &path) const {
  std::lock_guard<std::mutex> lock(locks_mutex_);

  auto it = write_locks_.find(path);
  CHECK(it != write_locks_.end());
  write_locks_.erase(it);
}

}  // namespace dist_clang
