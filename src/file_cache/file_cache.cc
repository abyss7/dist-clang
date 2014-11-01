#include <file_cache/file_cache.h>

#include <base/file_utils.h>
#include <base/hash.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <file_cache/manifest.pb.h>
#include <file_cache/manifest_utils.h>

#include <third_party/snappy/exported/snappy.h>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

FileCache::FileCache(const String &path, ui64 size, bool snappy)
    : path_(path),
      pool_(base::ThreadPool::TaskQueue::UNLIMITED, 1 + snappy),
      max_size_(size),
      cached_size_(0),
      database_(path, "direct"),
      snappy_(snappy) {
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

FileCache::FileCache(const String &path) : FileCache(path, UNLIMITED, false) {}

using namespace file_cache::string;

// static
HandledHash FileCache::Hash(const HandledSource &code,
                            const CommandLine &command_line,
                            const Version &version) {
  return HandledHash(base::Hexify(base::MakeHash(code)) + "-" +
                     base::Hexify(base::MakeHash(command_line, 4)) + "-" +
                     base::Hexify(base::MakeHash(version, 4)));
}

// static
UnhandledHash FileCache::Hash(const UnhandledSource &code,
                              const CommandLine &command_line,
                              const Version &version) {
  return UnhandledHash(base::Hexify(base::MakeHash(code)) + "-" +
                       base::Hexify(base::MakeHash(command_line, 4)) + "-" +
                       base::Hexify(base::MakeHash(version, 4)));
}

bool FileCache::FindByHash(const HandledHash &hash, Entry *entry) const {
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
      const String object_path = CommonPath(hash) + ".o";

      if (manifest.snappy()) {
        String error;

        String packed_content;
        if (!base::ReadFile(object_path, &packed_content, &error)) {
          LOG(CACHE_ERROR) << "Failed to read " << object_path << " : "
                           << error;
          return false;
        }

        if (!snappy::Uncompress(packed_content.data(), packed_content.size(),
                                &entry->object)) {
          LOG(CACHE_ERROR) << "Failed to unpack contents of " << object_path;
          return false;
        }
      } else {
        if (!base::ReadFile(object_path, &entry->object)) {
          return false;
        }
      }
    }

    if (manifest.deps()) {
      const String deps_path = CommonPath(hash) + ".d";
      if (!base::ReadFile(deps_path, &entry->deps)) {
        return false;
      }
    }
  }

  return true;
}

bool FileCache::Find(const HandledSource &code, const CommandLine &command_line,
                     const Version &version, Entry *entry) const {
  return FindByHash(Hash(code, command_line, version), entry);
}

bool FileCache::Find(const UnhandledSource &code,
                     const CommandLine &command_line, const Version &version,
                     Entry *entry) const {
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
    return FindByHash(HandledHash(hash), entry);
  }

  return false;
}

FileCache::Optional FileCache::Store(const HandledSource &code,
                                     const CommandLine &command_line,
                                     const Version &version,
                                     const Entry &entry) {
  pool_.Push([=] { DoStore(Hash(code, command_line, version), entry); });
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::Store(const UnhandledSource &code,
                                     const CommandLine &command_line,
                                     const Version &version,
                                     const List<String> &headers,
                                     const HandledHash &hash) {
  pool_.Push(
      [=] { DoStore(Hash(code, command_line, version), headers, hash); });
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::StoreNow(const HandledSource &code,
                                        const CommandLine &command_line,
                                        const Version &version,
                                        const Entry &entry) {
  DoStore(Hash(code, command_line, version), entry);
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::StoreNow(const UnhandledSource &code,
                                        const CommandLine &command_line,
                                        const Version &version,
                                        const List<String> &headers,
                                        const HandledHash &hash) {
  DoStore(Hash(code, command_line, version), headers, hash);
  return pool_.Push(std::bind(&FileCache::Clean, this));
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

  auto size = base::FileSize(manifest_path);
  if (!base::DeleteFile(manifest_path)) {
    result = false;
  } else {
    DCHECK(size <= cached_size_);
    cached_size_ -= size;
  }

  return result;
}

void FileCache::DoStore(const HandledHash &hash, const Entry &entry) {
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

  if (!entry.object.empty()) {
    const String object_path = CommonPath(hash) + ".o";
    String error;

    if (!snappy_) {
      if (!base::WriteFile(object_path, entry.object)) {
        RemoveEntry(manifest_path);
        LOG(CACHE_ERROR) << "Failed to save object to " << object_path;
        return;
      }
    } else {
      String packed_content;
      if (!snappy::Compress(entry.object.data(), entry.object.size(),
                            &packed_content)) {
        RemoveEntry(manifest_path);
        LOG(CACHE_ERROR) << "Failed to pack contents for " << object_path;
        return;
      }

      if (!base::WriteFile(object_path, packed_content, &error)) {
        RemoveEntry(manifest_path);
        LOG(CACHE_ERROR) << "Failed to write to " << object_path << " : "
                         << error;
        return;
      }

      manifest.set_snappy(true);
    }

    cached_size_ += base::FileSize(object_path);
  } else {
    manifest.set_object(false);
  }

  if (!entry.deps.empty()) {
    const String deps_path = CommonPath(hash) + ".d";

    if (!base::WriteFile(deps_path, entry.deps)) {
      RemoveEntry(manifest_path);
      LOG(CACHE_ERROR) << "Failed to save deps to " << deps_path;
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

  LOG(CACHE_VERBOSE) << "File is cached on path " << CommonPath(hash);
}

void FileCache::DoStore(UnhandledHash orig_hash, const List<String> &headers,
                        const HandledHash &hash) {
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

  String direct_hash = orig_hash;
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
    direct_hash += header_hash;
    manifest.add_headers(header);
  }

  direct_hash = base::Hexify(base::MakeHash(direct_hash));
  if (!database_.Set(direct_hash, hash)) {
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
}

void FileCache::Clean() {
  if (max_size_ != UNLIMITED) {
    while (cached_size_ > max_size_) {
      String first_path, second_path;

      {
        String error;
        if (!base::GetLeastRecentPath(path_, first_path, "[0-9a-f]", &error)) {
          LOG(CACHE_WARNING) << "Failed to get the recent path from " << path_
                             << " : " << error;
          LOG(CACHE_ERROR) << "Failed to clean the cache";
          break;
        }
      }
      {
        String error;
        if (!base::GetLeastRecentPath(first_path, second_path, "[0-9a-f]",
                                      &error)) {
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
