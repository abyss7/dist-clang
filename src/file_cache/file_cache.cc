#include <file_cache/file_cache.h>

#include <base/file_utils.h>
#include <base/hash.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <file_cache/manifest.pb.h>
#include <file_cache/manifest_utils.h>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

FileCache::FileCache(const String &path, ui64 size)
    : path_(path),
      pool_(base::ThreadPool::TaskQueue::UNLIMITED, 1),
      max_size_(size),
      cached_size_(0),
      database_(path, "direct") {
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
  return FindByHash(hash, entry);
}

bool FileCache::Find_Direct(const String &code, const String &command_line,
                            const String &version, Entry *entry) const {
  String hash = Hash(code, command_line, version);
  const String first_path = FirstPath(hash);
  const String second_path = SecondPath(hash);
  const String manifest_path = CommonPath(hash) + ".manifest";
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!file_cache::LoadManifest(manifest_path, &manifest)) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  for (const auto &header : manifest.headers()) {
    String header_hash;
    if (!base::HashFile(header, &header_hash)) {
      return false;
    }
    hash += header_hash;
  }
  hash = base::Hexify(base::MakeHash(hash));

  if (database_.Get(hash, &hash)) {
    return FindByHash(hash, entry);
  }

  return false;
}

FileCache::Optional FileCache::Store(const String &code,
                                     const String &command_line,
                                     const String &version,
                                     const Entry &entry) {
  auto &&hash = Hash(code, command_line, version);
  auto task = std::bind(&FileCache::DoStore, this, hash, entry);
  return pool_.Push(task);
}

FileCache::Optional FileCache::Store_Direct(const String &code,
                                            const String &command_line,
                                            const String &version,
                                            const List<String> &headers,
                                            const String &hash) {
  auto &&orig_hash = Hash(code, command_line, version);
  auto task =
      std::bind(&FileCache::DoStore_Direct, this, orig_hash, headers, hash);
  return pool_.Push(task);
}

void FileCache::StoreNow(const String &code, const String &command_line,
                         const String &version, const Entry &entry) {
  DoStore(Hash(code, command_line, version), entry);
}

void FileCache::StoreNow_Direct(const String &code, const String &command_line,
                                const String &version,
                                const List<String> &headers,
                                const String &hash) {
  DoStore_Direct(Hash(code, command_line, version), headers, hash);
}

bool FileCache::FindByHash(const String &hash, Entry *entry) const {
  const String first_path = FirstPath(hash);
  const String second_path = SecondPath(hash);
  const String manifest_path = CommonPath(hash) + ".manifest";
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!file_cache::LoadManifest(manifest_path, &manifest)) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  if (entry) {
    if (manifest.stderr()) {
      const String stderr_path = CommonPath(hash) + ".stderr";
      if (!base::ReadFile(stderr_path, &entry->stderr)) {
        return false;
      }
    }

    if (manifest.object()) {
      entry->object_path = CommonPath(hash) + ".o";
      if (!base::FileExists(entry->object_path)) {
        return false;
      }
    }

    if (manifest.deps()) {
      entry->deps_path = CommonPath(hash) + ".d";
      if (!base::FileExists(entry->deps_path)) {
        return false;
      }
    }
  }

  return true;
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
  WriteLock lock(this, manifest_path);

  if (!lock) {
    return;
  }

  // FIXME: refactor to portable solution.
  if (system((String("mkdir -p ") + SecondPath(hash)).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << SecondPath(hash);
    return;
  }

  proto::Manifest manifest;
  if (!entry.stderr.empty()) {
    const String stderr_path = CommonPath(hash) + ".stderr";
    if (!base::WriteFile(stderr_path, entry.stderr)) {
      RemoveEntry(manifest_path);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path;
      return;
    }
    manifest.set_stderr(true);
    cached_size_ += base::FileSize(stderr_path);
  }

  if (!entry.object_path.empty()) {
    const String object_path = CommonPath(hash) + ".o";
    String error;
    bool result;
    if (entry.move_object) {
      result = base::LinkFile(entry.object_path, object_path, &error);
      base::DeleteFile(entry.object_path);
    } else {
      result = base::CopyFile(entry.object_path, object_path, &error);
    }
    if (!result) {
      RemoveEntry(manifest_path);
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
    bool result;
    if (entry.move_deps) {
      result = base::LinkFile(entry.deps_path, deps_path, &error);
      base::DeleteFile(entry.deps_path);
    } else {
      result = base::CopyFile(entry.deps_path, deps_path, &error);
    }
    if (!result) {
      RemoveEntry(manifest_path);
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
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path;
    return;
  }
  cached_size_ += base::FileSize(manifest_path);

  utime(manifest_path.c_str(), nullptr);
  utime(SecondPath(hash).c_str(), nullptr);
  utime(FirstPath(hash).c_str(), nullptr);

  LOG(CACHE_VERBOSE) << "File " << entry.object_path << " is cached on path "
                     << CommonPath(hash);

  lock.Unlock();
  Clean();
}

void FileCache::DoStore_Direct(String orig_hash, const List<String> &headers,
                               const String &hash) {
  const String manifest_path = CommonPath(orig_hash) + ".manifest";
  WriteLock lock(this, manifest_path);

  if (!lock) {
    LOG(CACHE_ERROR) << "Failed to lock " << manifest_path << " for writing";
    return;
  }

  // FIXME: refactor to portable solution.
  if (system((String("mkdir -p ") + SecondPath(orig_hash)).c_str()) == -1) {
    // "mkdir -p" doesn't fail if the path already exists.
    LOG(CACHE_ERROR) << "Failed to `mkdir -p` for " << SecondPath(orig_hash);
    return;
  }

  proto::Manifest manifest;
  manifest.set_stderr(false);
  manifest.set_object(false);
  manifest.set_deps(false);
  for (const auto &header : headers) {
    String header_hash, error;
    if (!base::HashFile(header, &header_hash, {"__DATE__", "__TIME__"},
                        &error)) {
      LOG(CACHE_ERROR) << "Failed to hash " << header << ": " << error;
      return;
    }
    orig_hash += header_hash;
    manifest.add_headers(header);
  }

  orig_hash = base::Hexify(base::MakeHash(orig_hash));
  if (!database_.Set(orig_hash, hash)) {
    return;
  }

  if (!file_cache::SaveManifest(manifest_path, manifest)) {
    RemoveEntry(manifest_path);
    return;
  }
  cached_size_ += base::FileSize(manifest_path);

  utime(manifest_path.c_str(), nullptr);
  utime(SecondPath(hash).c_str(), nullptr);
  utime(FirstPath(hash).c_str(), nullptr);

  lock.Unlock();
  Clean();
}

void FileCache::Clean() {
  if (max_size_ != UNLIMITED) {
    DCHECK(
        cached_size_ < base::CalculateDirectorySize(path_)
            ? base::CalculateDirectorySize(path_) - cached_size_ < 10 * 2 << 20
            : cached_size_ - base::CalculateDirectorySize(path_) < 10 * 2 << 20)

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

        WriteLock lock(this, manifest_path);
        if (lock) {
          should_break = !RemoveEntry(manifest_path);
        }
      }

      if (should_break) {
        break;
      }
    }
  }
}

FileCache::ReadLock::ReadLock(const FileCache *file_cache, const String &path)
    : cache_(file_cache), path_(path) {
  if (!base::FileExists(path)) {
    return;
  }

  std::lock_guard<std::mutex> lock(cache_->locks_mutex_);

  if (cache_->write_locks_.find(path) != cache_->write_locks_.end()) {
    return;
  }

  auto it = cache_->read_locks_.find(path);
  if (it == cache_->read_locks_.end()) {
    it = cache_->read_locks_.emplace(path, 0).first;
  }
  it->second++;

  locked_ = true;
}

void FileCache::ReadLock::Unlock() {
  if (locked_) {
    std::lock_guard<std::mutex> lock(cache_->locks_mutex_);

    auto it = cache_->read_locks_.find(path_);
    CHECK(it != cache_->read_locks_.end() && it->second > 0);
    it->second--;

    if (it->second == 0) {
      cache_->read_locks_.erase(it);
    }

    locked_ = false;
  }
}

FileCache::WriteLock::WriteLock(const FileCache *file_cache, const String &path)
    : cache_(file_cache), path_(path) {
  std::lock_guard<std::mutex> lock(cache_->locks_mutex_);

  if (cache_->write_locks_.find(path) != cache_->write_locks_.end() ||
      cache_->read_locks_.find(path) != cache_->read_locks_.end()) {
    return;
  }

  cache_->write_locks_.insert(path);
  locked_ = true;
}

void FileCache::WriteLock::Unlock() {
  if (locked_) {
    std::lock_guard<std::mutex> lock(cache_->locks_mutex_);

    auto it = cache_->write_locks_.find(path_);
    CHECK(it != cache_->write_locks_.end());
    cache_->write_locks_.erase(it);

    locked_ = false;
  }
}

}  // namespace dist_clang
