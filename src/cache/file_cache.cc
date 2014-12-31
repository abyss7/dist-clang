#include <cache/file_cache.h>

#include <base/file/file.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/manifest.pb.h>
#include <cache/manifest_utils.h>

#include <third_party/snappy/exported/snappy.h>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

String ReplaceTildeInPath(const String& path) {
  if (path[0] == '~' && path[1] == '/') {
    return String(base::GetHomeDir()) + path.substr(1);
  }
  return path;
}

}  // namespace

namespace cache {

FileCache::FileCache(const String& path, ui64 size, bool snappy)
    : path_(ReplaceTildeInPath(path)),
      max_size_(size),
      cached_size_(0),
      snappy_(snappy),
      pool_(base::ThreadPool::TaskQueue::UNLIMITED, 1 + snappy) {
}

FileCache::FileCache(const String& path) : FileCache(path, UNLIMITED, false) {
}

bool FileCache::Run() {
  String error;
  if (!base::CreateDirectory(path_, &error)) {
    LOG(CACHE_ERROR) << "Failed to create directory " << path_ << " : "
                     << error;
    return false;
  }

  database_.reset(new Database(path_, "direct"));

  if (max_size_ != UNLIMITED) {
    cached_size_ =
        base::CalculateDirectorySize(path_, &error) - database_->SizeOnDisk();
    if (!error.empty()) {
      max_size_ = UNLIMITED;
      LOG(CACHE_WARNING)
          << "Error occured during calculation of the cache size: " << error;
    }
  }

  pool_.Run();

  return true;
}

using namespace string;

// static
HandledHash FileCache::Hash(HandledSource code, CommandLine command_line,
                            Version version) {
  return HandledHash(base::Hexify(code.str.Hash()) + "-" +
                     base::Hexify(command_line.str.Hash(4)) + "-" +
                     base::Hexify(version.str.Hash(4)));
}

// static
UnhandledHash FileCache::Hash(UnhandledSource code, CommandLine command_line,
                              Version version) {
  return UnhandledHash(base::Hexify(code.str.Hash()) + "-" +
                       base::Hexify(command_line.str.Hash(4)) + "-" +
                       base::Hexify(version.str.Hash(4)));
}

bool FileCache::Find(const HandledSource& code, const CommandLine& command_line,
                     const Version& version, Entry* entry) const {
  return FindByHash(Hash(code, command_line, version), entry);
}

bool FileCache::Find(const UnhandledSource& code,
                     const CommandLine& command_line, const Version& version,
                     Entry* entry) const {
  Immutable hash1 = Hash(code, command_line, version);
  const String first_path = FirstPath(hash1);
  const String second_path = SecondPath(hash1);
  const String manifest_path = CommonPath(hash1) + ".manifest";
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!LoadManifest(manifest_path, &manifest)) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  Immutable::Rope hash_rope = {hash1};
  for (const auto& header : manifest.headers()) {
    Immutable header_hash;
    if (!base::File::Hash(header, &header_hash)) {
      return false;
    }
    hash_rope.push_back(header_hash);
  }
  Immutable hash2 = base::Hexify(Immutable(hash_rope).Hash()), hash3;

  DCHECK(database_);
  if (database_->Get(hash2, &hash3)) {
    return FindByHash(HandledHash(hash3), entry);
  }

  return false;
}

FileCache::Optional FileCache::Store(const HandledSource& code,
                                     const CommandLine& command_line,
                                     const Version& version,
                                     const Entry& entry) {
  pool_.Push([=] { DoStore(Hash(code, command_line, version), entry); });
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::Store(const UnhandledSource& code,
                                     const CommandLine& command_line,
                                     const Version& version,
                                     const List<String>& headers,
                                     const HandledHash& hash) {
  pool_.Push(
      [=] { DoStore(Hash(code, command_line, version), headers, hash); });
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::StoreNow(const HandledSource& code,
                                        const CommandLine& command_line,
                                        const Version& version,
                                        const Entry& entry) {
  DoStore(Hash(code, command_line, version), entry);
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

FileCache::Optional FileCache::StoreNow(const UnhandledSource& code,
                                        const CommandLine& command_line,
                                        const Version& version,
                                        const List<String>& headers,
                                        const HandledHash& hash) {
  DoStore(Hash(code, command_line, version), headers, hash);
  return pool_.Push(std::bind(&FileCache::Clean, this));
}

bool FileCache::FindByHash(const HandledHash& hash, Entry* entry) const {
  const String first_path = FirstPath(hash);
  const String second_path = SecondPath(hash);
  const String manifest_path = CommonPath(hash) + ".manifest";
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!LoadManifest(manifest_path, &manifest)) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  utime(second_path.c_str(), nullptr);
  utime(first_path.c_str(), nullptr);

  if (entry) {
    if (manifest.stderr()) {
      const String stderr_path = CommonPath(hash) + ".stderr";
      if (!base::File::Read(stderr_path, &entry->stderr)) {
        return false;
      }
    }

    if (manifest.object()) {
      const String object_path = CommonPath(hash) + ".o";

      if (manifest.snappy()) {
        String error;

        Immutable packed_content;
        if (!base::File::Read(object_path, &packed_content, &error)) {
          LOG(CACHE_ERROR) << "Failed to read " << object_path << " : "
                           << error;
          return false;
        }

        String object_str;
        if (!snappy::Uncompress(packed_content.data(), packed_content.size(),
                                &object_str)) {
          LOG(CACHE_ERROR) << "Failed to unpack contents of " << object_path;
          return false;
        }

        entry->object = std::move(object_str);
      } else {
        if (!base::File::Read(object_path, &entry->object)) {
          return false;
        }
      }
    }

    if (manifest.deps()) {
      const String deps_path = CommonPath(hash) + ".d";
      if (!base::File::Read(deps_path, &entry->deps)) {
        return false;
      }
    }
  }

  return true;
}

bool FileCache::RemoveEntry(const String& manifest_path) {
  const String common_path =
      manifest_path.substr(0, manifest_path.size() - sizeof(".manifest") + 1);
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
  bool result = true;

  if (base::File::Exists(object_path)) {
    auto size = base::File::Size(object_path);
    if (!base::File::Delete(object_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  if (base::File::Exists(deps_path)) {
    auto size = base::File::Size(deps_path);
    if (!base::File::Delete(deps_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  if (base::File::Exists(stderr_path)) {
    auto size = base::File::Size(stderr_path);
    if (!base::File::Delete(stderr_path)) {
      result = false;
    } else {
      DCHECK(size <= cached_size_);
      cached_size_ -= size;
    }
  }

  auto size = base::File::Size(manifest_path);
  if (!base::File::Delete(manifest_path)) {
    result = false;
  } else {
    DCHECK(size <= cached_size_);
    cached_size_ -= size;
  }

  return result;
}

void FileCache::DoStore(const HandledHash& hash, const Entry& entry) {
  const String manifest_path = CommonPath(hash) + ".manifest";
  WriteLock lock(this, manifest_path);

  if (!lock) {
    return;
  }

  if (!base::CreateDirectory(SecondPath(hash))) {
    LOG(CACHE_ERROR) << "Failed to create directory " << SecondPath(hash);
    return;
  }

  proto::Manifest manifest;
  if (!entry.stderr.empty()) {
    const String stderr_path = CommonPath(hash) + ".stderr";

    if (!base::File::Write(stderr_path, entry.stderr)) {
      RemoveEntry(manifest_path);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path;
      return;
    }
    manifest.set_stderr(true);

    cached_size_ += base::File::Size(stderr_path);
  }

  if (!entry.object.empty()) {
    const String object_path = CommonPath(hash) + ".o";
    String error;

    if (!snappy_) {
      if (!base::File::Write(object_path, entry.object)) {
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

      if (!base::File::Write(object_path, std::move(packed_content), &error)) {
        RemoveEntry(manifest_path);
        LOG(CACHE_ERROR) << "Failed to write to " << object_path << " : "
                         << error;
        return;
      }

      manifest.set_snappy(true);
    }

    cached_size_ += base::File::Size(object_path);
  } else {
    manifest.set_object(false);
  }

  if (!entry.deps.empty()) {
    const String deps_path = CommonPath(hash) + ".d";

    if (!base::File::Write(deps_path, entry.deps)) {
      RemoveEntry(manifest_path);
      LOG(CACHE_ERROR) << "Failed to save deps to " << deps_path;
      return;
    }

    cached_size_ += base::File::Size(deps_path);
  } else {
    manifest.set_deps(false);
  }

  if (!SaveManifest(manifest_path, manifest)) {
    RemoveEntry(manifest_path);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path;
    return;
  }
  cached_size_ += base::File::Size(manifest_path);

  utime(manifest_path.c_str(), nullptr);
  utime(SecondPath(hash).c_str(), nullptr);
  utime(FirstPath(hash).c_str(), nullptr);

  LOG(CACHE_VERBOSE) << "File is cached on path " << CommonPath(hash);
}

void FileCache::DoStore(UnhandledHash orig_hash, const List<String>& headers,
                        const HandledHash& hash) {
  // We have to store manifest on the path based only on the hash of unhandled
  // source code. Otherwise, we won't be able to get list of the dependent
  // headers, while checking the direct cache. Such approach has a little
  // drawback, because the changes in the dependent headers will make a
  // false-positive direct cache hit, followed by true cache miss.
  const String manifest_path = CommonPath(orig_hash) + ".manifest";
  WriteLock lock(this, manifest_path);

  if (!lock) {
    LOG(CACHE_ERROR) << "Failed to lock " << manifest_path << " for writing";
    return;
  }

  if (!base::CreateDirectory(SecondPath(orig_hash))) {
    LOG(CACHE_ERROR) << "Failed to create directory " << SecondPath(orig_hash);
    return;
  }

  Immutable::Rope hash_rope = {orig_hash};
  proto::Manifest manifest;
  manifest.set_stderr(false);
  manifest.set_object(false);
  manifest.set_deps(false);
  for (const auto& header : headers) {
    String error;
    Immutable header_hash;
    if (!base::File::Hash(header, &header_hash, {"__DATE__"_l, "__TIME__"_l},
                          &error)) {
      LOG(CACHE_ERROR) << "Failed to hash " << header << ": " << error;
      return;
    }
    hash_rope.push_back(header_hash);
    manifest.add_headers(header);
  }

  auto direct_hash = base::Hexify(Immutable(hash_rope).Hash());
  DCHECK(database_);
  if (!database_->Set(direct_hash, hash)) {
    return;
  }

  if (!SaveManifest(manifest_path, manifest)) {
    RemoveEntry(manifest_path);
    return;
  }
  cached_size_ += base::File::Size(manifest_path);

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

FileCache::ReadLock::ReadLock(const FileCache* file_cache, const String& path)
    : cache_(file_cache), path_(path) {
  if (!base::File::Exists(path)) {
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

FileCache::WriteLock::WriteLock(const FileCache* file_cache, const String& path)
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

}  // namespace cache
}  // namespace dist_clang
