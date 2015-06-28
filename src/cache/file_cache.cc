#include <cache/file_cache.h>

#include <base/file/file.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <cache/manifest.pb.h>
#include <cache/manifest_utils.h>
#include <perf/stat_service.h>

#include <third_party/snappy/exported/snappy.h>
#include STL(regex)

#include <clang/Basic/Version.h>

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
    : path_(ReplaceTildeInPath(path)), snappy_(snappy), max_size_(size) {
}

FileCache::FileCache(const String& path) : FileCache(path, UNLIMITED, false) {
}

FileCache::~FileCache() {
  resetter_.reset();
  new_entries_.reset();
}

bool FileCache::Run(ui64 clean_period) {
  String error;
  if (!base::CreateDirectory(path_, &error)) {
    LOG(CACHE_ERROR) << "Failed to create directory " << path_ << " : "
                     << error;
    return false;
  }

  database_.reset(new Database(path_, "direct"));

  CHECK(clean_period > 0);

  base::WorkerPool::SimpleWorker worker;
  if (max_size_ == UNLIMITED) {
    new_entries_.reset(new EntryList);

    worker = [this, clean_period](const Atomic<bool>& is_shutting_down) {
      while (!is_shutting_down) {
        std::this_thread::sleep_for(std::chrono::seconds(clean_period));
        new_entries_.reset(new EntryList);
      }
    };
  } else {
    base::WalkDirectory(
        path_, [this](const String& file_path, ui64 mtime, ui64) {
          std::regex regex("([a-f0-9]{32}-[a-f0-9]{8}-[a-f0-9]{8})\\.manifest");
          std::cmatch match;
          if (std::regex_search(file_path.c_str(), match, regex) &&
              match.size() > 1 && match[1].matched) {
            auto hash = string::Hash(String(match[1]));
            auto size = GetEntrySize(hash);

            if (size) {
              auto entry = entries_.emplace(hash, TimeSizePair(mtime, size));
              mtimes_.emplace(mtime, entry.first);
              cache_size_ += size;

              CHECK(entry.second);  // There should be no duplicate entries.
              LOG(CACHE_VERBOSE) << hash.str << " is considered";
            } else {
              RemoveEntry(hash, true);
              LOG(CACHE_WARNING) << hash.str << " is broken and removed";
            }
          }
        });

    new_entries_.reset(new EntryList, new_entries_deleter_);

    worker = [this, clean_period](const Atomic<bool>& is_shutting_down) {
      while (!is_shutting_down) {
        std::this_thread::sleep_for(std::chrono::seconds(clean_period));
        new_entries_.reset(new EntryList, new_entries_deleter_);
      }
    };
  }
  resetter_->AddWorker(worker);
  cleaner_.Run();

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
                       base::Hexify((version.str + "\n"_l +
                                     clang::getClangFullVersion()).Hash(4)));
}

bool FileCache::Find(const HandledSource& code, const CommandLine& command_line,
                     const Version& version, Entry* entry) const {
  return FindByHash(Hash(code, command_line, version), entry);
}

bool FileCache::Find(const UnhandledSource& code,
                     const CommandLine& command_line, const Version& version,
                     Entry* entry) const {
  auto hash1 = Hash(code, command_line, version);
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
  new_entries_->Append({time(nullptr), hash1});

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

void FileCache::Store(const UnhandledSource& code,
                      const CommandLine& command_line, const Version& version,
                      const List<String>& headers, const HandledHash& hash) {
  DoStore(Hash(code, command_line, version), headers, hash);
}

void FileCache::Store(const HandledSource& code,
                      const CommandLine& command_line, const Version& version,
                      const Entry& entry) {
  DoStore(Hash(code, command_line, version), entry);
}

bool FileCache::FindByHash(const HandledHash& hash, Entry* entry) const {
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
  new_entries_->Append({time(nullptr), hash});

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

ui64 FileCache::GetEntrySize(string::Hash hash) const {
  const String common_path = CommonPath(hash);
  const String manifest_path = common_path + ".manifest";
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
  ui64 result = 0u;

  proto::Manifest manifest;
  if (!LoadManifest(manifest_path, &manifest)) {
    LOG(CACHE_VERBOSE) << "Can't load manifest for " << hash.str;
    return 0u;
  }

  if (manifest.object()) {
    if (!base::File::Exists(object_path)) {
      LOG(CACHE_VERBOSE) << object_path << " should exist, but not found!";
      return 0u;
    }
    result += base::File::Size(object_path);
  }

  if (manifest.deps()) {
    if (!base::File::Exists(deps_path)) {
      LOG(CACHE_VERBOSE) << deps_path << " should exist, but not found!";
      return 0u;
    }
    result += base::File::Size(deps_path);
  }

  if (manifest.stderr()) {
    if (!base::File::Exists(stderr_path)) {
      LOG(CACHE_VERBOSE) << stderr_path << " should exist, but not found!";
      return 0u;
    }
    result += base::File::Size(stderr_path);
  }

  result += base::File::Size(manifest_path);

  return result;
}

bool FileCache::RemoveEntry(string::Hash hash, bool possibly_broken) {
  auto entry_it = entries_.find(hash);
  bool has_entry = entry_it != entries_.end();

  CHECK(possibly_broken || has_entry);

  const String common_path = CommonPath(hash);
  const String manifest_path = common_path + ".manifest";
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
  bool result = true;
  auto entry_size = has_entry ? entry_it->second.second : 0u;

  if (has_entry) {
    entries_.erase(entry_it);
  }

  proto::Manifest manifest;
  if (!LoadManifest(manifest_path, &manifest)) {
    result = false;
  }

  String error;

  if (base::File::Exists(object_path)) {
    if (!base::File::Delete(object_path, &error)) {
      entry_size -= base::File::Size(object_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << object_path << ": " << error;
    }
  } else {
    DCHECK(possibly_broken || !manifest.object());
  }

  if (base::File::Exists(deps_path)) {
    if (!base::File::Delete(deps_path, &error)) {
      entry_size -= base::File::Size(deps_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << deps_path << ": " << error;
    }
  } else {
    DCHECK(possibly_broken || !manifest.deps());
  }

  if (base::File::Exists(stderr_path)) {
    if (!base::File::Delete(stderr_path, &error)) {
      entry_size -= base::File::Size(stderr_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << stderr_path << ": " << error;
    }
  } else {
    DCHECK(possibly_broken || !manifest.stderr());
  }

  if (!base::File::Delete(manifest_path, &error)) {
    entry_size -= base::File::Size(manifest_path);
    result = false;
    LOG(CACHE_WARNING) << "Failed to delete " << manifest_path << ": " << error;
  }

  if (has_entry) {
    cache_size_ -= entry_size;
    STAT(CACHE_SIZE_CLEANED, entry_size);
  }

  return result;
}

void FileCache::DoStore(const HandledHash& hash, Entry entry) {
  auto manifest_path = CommonPath(hash) + ".manifest";
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
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path;
      return;
    }
    manifest.set_stderr(true);
  }

  if (!entry.object.empty()) {
    const String object_path = CommonPath(hash) + ".o";
    String error;

    if (!snappy_) {
      if (!base::File::Write(object_path, entry.object)) {
        RemoveEntry(hash);
        LOG(CACHE_ERROR) << "Failed to save object to " << object_path;
        return;
      }
    } else {
      String packed_content;
      if (!snappy::Compress(entry.object.data(), entry.object.size(),
                            &packed_content)) {
        RemoveEntry(hash);
        LOG(CACHE_ERROR) << "Failed to pack contents for " << object_path;
        return;
      }

      if (!base::File::Write(object_path, std::move(packed_content), &error)) {
        RemoveEntry(hash);
        LOG(CACHE_ERROR) << "Failed to write to " << object_path << " : "
                         << error;
        return;
      }

      manifest.set_snappy(true);
    }
  } else {
    manifest.set_object(false);
  }

  if (!entry.deps.empty()) {
    const String deps_path = CommonPath(hash) + ".d";

    if (!base::File::Write(deps_path, entry.deps)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save deps to " << deps_path;
      return;
    }
  } else {
    manifest.set_deps(false);
  }

  if (!SaveManifest(manifest_path, manifest)) {
    RemoveEntry(hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), hash});

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
    RemoveEntry(orig_hash);
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), orig_hash});
}

void FileCache::Clean(UniquePtr<EntryList> list) {
  DCHECK(max_size_ != UNLIMITED);

  while (auto new_entry = list->Pop()) {
    auto entry_it = entries_.find(new_entry->second);
    if (entry_it != entries_.end()) {
      // Update mtime of an existing entry.
      auto mtime_its = mtimes_.equal_range(entry_it->second.first);
      auto mtime_it = mtime_its.first;
      while (mtime_it != mtime_its.second) {
        if (mtime_it->second == entry_it) {
          break;
        }
        ++mtime_it;
      }
      CHECK(mtime_it != mtime_its.second);

      mtimes_.erase(mtime_it);
      auto old_mtime = entry_it->second.first;
      entry_it->second.first = new_entry->first;
      mtimes_.emplace(new_entry->first, entry_it);

      LOG(CACHE_VERBOSE) << entry_it->first.str << " wasn't used for "
                         << (new_entry->first - old_mtime) << " seconds";
    } else {
      // Insert new entry.
      auto size = GetEntrySize(new_entry->second);
      auto new_entry_it = entries_.emplace(
          new_entry->second, TimeSizePair(new_entry->first, size));
      mtimes_.emplace(new_entry->first, new_entry_it.first);
      cache_size_ += size;
      STAT(CACHE_SIZE_ADDED, size);

      CHECK(size);
      CHECK(new_entry_it.second);
    }
  }

  while (cache_size_ > max_size_) {
    auto mtime_it = mtimes_.begin();
    auto entry_it = mtime_it->second;

    auto manifest_path = CommonPath(entry_it->first) + ".manifest";
    WriteLock lock(this, manifest_path);
    if (lock) {
      LOG(CACHE_VERBOSE) << "Cache overuse is " << (cache_size_ - max_size_)
                         << " bytes: removing " << entry_it->first.str;
      DCHECK(RemoveEntry(entry_it->first));
    }

    mtimes_.erase(mtime_it);
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
