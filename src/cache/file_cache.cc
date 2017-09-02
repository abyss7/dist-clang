#include <cache/file_cache.h>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <perf/stat_service.h>

#include <third_party/snappy/exported/snappy.h>
#include STL(regex)

#include <clang/Basic/Version.h>

#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

Path ReplaceTildeInPath(const String& path) {
  // FIXME: try to handle |Path| instead of |String|
  if (path[0] == '~' && path[1] == Path::preferred_separator) {
    return base::GetHomeDir() / path.substr(2);
  }
  return path;
}

String HashCombine(const Immutable& source, const cache::ExtraFiles& files) {
  if (files.empty()) {  // we want hash to be compatible with previous versions
    return base::Hexify(source.Hash());
  }
  Immutable::Rope hashes_string{source.Hash()};
  for (auto&& index_file_pair : files) {
    std::stringstream ss;
    ss << index_file_pair.first;
    hashes_string.emplace_back(Immutable::WrapString(ss.str()).Hash(4));
    hashes_string.emplace_back(index_file_pair.second.Hash());
  }
  return base::Hexify(Immutable(hashes_string).Hash());
}

}  // namespace

namespace cache {

FileCache::FileCache(const Path& path, ui64 size, bool snappy, bool store_index)
    : path_(ReplaceTildeInPath(path)),
      snappy_(snappy),
      store_index_(store_index),
      max_size_(size) {}

FileCache::FileCache(const Path& path)
    : FileCache(path, UNLIMITED, false, false) {}

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

  database_.reset(new LevelDB(path_, "direct"));
  if (store_index_) {
    entries_.reset(new SQLite(path_, "index"));
  } else {
    entries_.reset(new SQLite);
  }

  CHECK(clean_period > 0);

  entries_->BeginTransaction();
  base::WalkDirectory(path_, [this](const String& file_path, ui64 mtime, ui64) {
    std::regex regex("([a-f0-9]{32}-[a-f0-9]{8}-[a-f0-9]{8})\\.manifest$");
    std::cmatch match;
    if (!std::regex_search(file_path.c_str(), match, regex) ||
        match.size() < 2 || !match[1].matched) {
      return;
    }

    auto hash = string::Hash(String(match[1]));
    ui64 size = 0u;
    bool from_index = false;

    if (!Migrate(hash)) {
      RemoveEntry(hash);
    } else {
      from_index = GetEntrySize(hash, &size);
    }

    if (size) {
      CHECK(from_index ||
            entries_->Set(hash.str,
                          std::make_tuple(mtime, size, kManifestVersion)));

      cache_size_ += size;
      LOG(CACHE_INFO) << hash.str << " is considered";
    } else {
      // When an entry has a zero size, it's not useful even if it's correct.
      RemoveEntry(hash);
    }
  });
  entries_->EndTransaction();

  new_entries_.reset(new EntryList, new_entries_deleter_);

  base::WorkerPool::SimpleWorker worker =
      [this, clean_period](const base::WorkerPool& pool) {
        while (!pool.WaitUntilShutdown(Seconds(clean_period))) {
          new_entries_.reset(new EntryList, new_entries_deleter_);
        }
      };
  resetter_->AddWorker("Cache Resetter Worker"_l, worker);
  cleaner_.Run();

  return true;
}

using namespace string;

// static
HandledHash FileCache::Hash(HandledSource code, const ExtraFiles& extra_files,
                            CommandLine command_line, Version version) {
  return HandledHash(HashCombine(code.str, extra_files) + "-" +
                     base::Hexify(command_line.str.Hash(4)) + "-" +
                     base::Hexify(version.str.Hash(4)));
}

// static
UnhandledHash FileCache::Hash(UnhandledSource code,
                              const ExtraFiles& extra_files,
                              CommandLine command_line, Version version) {
  return UnhandledHash(
      HashCombine(code.str, extra_files) + "-" +
      base::Hexify(command_line.str.Hash(4)) + "-" +
      base::Hexify(
          (version.str + "\n"_l + clang::getClangFullVersion()).Hash(4)));
}

bool FileCache::Find(UnhandledSource code, const ExtraFiles& extra_files,
                     CommandLine command_line, Version version,
                     const Path& current_dir, Entry* entry) const {
  DCHECK(entry);

  auto unhandled_hash = Hash(code, extra_files, command_line, version);
  const auto manifest_path =
      AppendExtension(CommonPath(unhandled_hash), base::kExtManifest);
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest) || !manifest.has_direct()) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), unhandled_hash});

  Immutable::Rope hash_rope = {unhandled_hash};
  for (const auto& header : manifest.direct().headers()) {
    Immutable header_hash;
    const Path header_path =
        Path(header).is_absolute() ? Path(header) : current_dir / header;
    if (!base::File::Hash(header_path, &header_hash)) {
      return false;
    }
    hash_rope.push_back(header_hash);
  }
  Immutable hash_with_headers = base::Hexify(Immutable(hash_rope).Hash());
  Immutable handled_hash;

  DCHECK(database_);
  if (database_->Get(hash_with_headers, &handled_hash)) {
    return Find(HandledHash(handled_hash), entry);
  }

  return false;
}

bool FileCache::Find(HandledHash hash, Entry* entry) const {
  DCHECK(entry);

  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest) || !manifest.has_v1()) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), hash});

  ui64 size = 0;

  if (manifest.v1().err()) {
    const auto stderr_path =
        AppendExtension(CommonPath(hash), base::kExtStderr);
    if (!base::File::Read(stderr_path, &entry->stderr)) {
      return false;
    }
    size += entry->stderr.size();
  }

  if (manifest.v1().obj()) {
    const auto object_path =
        AppendExtension(CommonPath(hash), base::kExtObject);

    if (manifest.v1().snappy()) {
      String error;

      Immutable packed_content;
      if (!base::File::Read(object_path, &packed_content, &error)) {
        LOG(CACHE_ERROR) << "Failed to read " << object_path << " : " << error;
        return false;
      }
      size += packed_content.size();

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
      size += entry->object.size();
    }
  }

  if (manifest.v1().dep()) {
    const auto deps_path = AppendExtension(CommonPath(hash), base::kExtDeps);
    if (!base::File::Read(deps_path, &entry->deps)) {
      return false;
    }
    size += entry->deps.size();
  }

  return manifest.v1().has_size() && manifest.v1().size() == size;
}

void FileCache::Store(UnhandledSource code, const ExtraFiles& extra_files,
                      CommandLine command_line, Version version,
                      const List<String>& headers,
                      const List<String>& preprocessed_headers,
                      const Path& current_dir, string::HandledHash hash) {
  DoStore(Hash(code, extra_files, command_line, version), headers,
          preprocessed_headers, current_dir, hash);
}

void FileCache::Store(string::HandledHash hash, Entry entry) {
  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  WriteLock lock(this, manifest_path);
  String error;

  if (!lock) {
    return;
  }

  if (!base::CreateDirectory(SecondPath(hash))) {
    LOG(CACHE_ERROR) << "Failed to create directory " << SecondPath(hash);
    return;
  }

  proto::Manifest manifest;
  manifest.set_version(kManifestVersion);

  manifest.mutable_v1()->set_err(!entry.stderr.empty());
  if (!entry.stderr.empty()) {
    const auto stderr_path =
        AppendExtension(CommonPath(hash), base::kExtStderr);

    if (!base::File::Write(stderr_path, entry.stderr, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path << ": "
                       << error;
      return;
    }
  }

  ui64 object_size = 0;

  manifest.mutable_v1()->set_obj(!entry.object.empty());
  if (!entry.object.empty()) {
    const auto object_path =
        AppendExtension(CommonPath(hash), base::kExtObject);

    if (!snappy_) {
      object_size = entry.object.size();

      if (!base::File::Write(object_path, entry.object, &error)) {
        RemoveEntry(hash);
        LOG(CACHE_ERROR) << "Failed to save object to " << object_path << ": "
                         << error;
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

      object_size = packed_content.size();

      if (!base::File::Write(object_path, std::move(packed_content), &error)) {
        RemoveEntry(hash);
        LOG(CACHE_ERROR) << "Failed to write to " << object_path << ": "
                         << error;
        return;
      }

      manifest.mutable_v1()->set_snappy(true);
    }
  }

  manifest.mutable_v1()->set_dep(!entry.deps.empty());
  if (!entry.deps.empty()) {
    const auto deps_path = AppendExtension(CommonPath(hash), base::kExtDeps);

    if (!base::File::Write(deps_path, entry.deps, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save deps to " << deps_path << ": "
                       << error;
      return;
    }
  }

  manifest.mutable_v1()->set_size(entry.stderr.size() + object_size +
                                  entry.deps.size());

  if (!base::SaveToFile(manifest_path, manifest, &error)) {
    RemoveEntry(hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), hash});

  LOG(CACHE_VERBOSE) << "File is cached on path " << CommonPath(hash);
}

bool FileCache::GetEntrySize(string::Hash hash, ui64* size) const {
  DCHECK(size);

  const auto common_prefix = CommonPath(hash);
  const auto manifest_path = AppendExtension(common_prefix, base::kExtManifest);

  SQLite::Value entry;
  if (entries_ && entries_->Get(hash.str, &entry)) {
    *size = std::get<SQLite::SIZE>(entry);
    return true;
  }

  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest)) {
    LOG(CACHE_WARNING) << "Can't load manifest for " << hash.str;
    *size = 0u;
    return false;
  }

  *size = manifest.v1().size() + base::File::Size(manifest_path);
  return false;
}

bool FileCache::RemoveEntry(string::Hash hash) {
  String error;
  SQLite::Value entry;
  bool has_entry = entries_->Get(hash.str, &entry);
  const String common_path = CommonPath(hash);
  const String manifest_path = common_path + ".manifest";
  const String object_path = common_path + ".o";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
  bool result = true;
  auto entry_size = has_entry ? std::get<SQLite::SIZE>(entry) : 0u;

  if (has_entry) {
    entries_->Delete(hash.str);
  } else {
    LOG(CACHE_WARNING) << "Removing unconsidered entry: " << hash.str;
  }

  if (base::File::Exists(object_path)) {
    if (!base::File::Delete(object_path, &error)) {
      entry_size -= base::File::Size(object_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << object_path << ": " << error;
    }
  }

  if (base::File::Exists(deps_path)) {
    if (!base::File::Delete(deps_path, &error)) {
      entry_size -= base::File::Size(deps_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << deps_path << ": " << error;
    }
  }

  if (base::File::Exists(stderr_path)) {
    if (!base::File::Delete(stderr_path, &error)) {
      entry_size -= base::File::Size(stderr_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << stderr_path << ": " << error;
    }
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

void FileCache::DoStore(UnhandledHash orig_hash, const List<String>& headers,
                        const List<String>& preprocessed_headers,
                        const Path& current_dir, const HandledHash& hash) {
  // We have to store manifest on the path based only on the hash of unhandled
  // source code. Otherwise, we won't be able to get list of the dependent
  // headers, while checking the direct cache. Such approach has a little
  // drawback, because the changes in the dependent headers will make a
  // false-positive direct cache hit, followed by true cache miss.
  const auto manifest_path =
      AppendExtension(CommonPath(orig_hash), base::kExtManifest);
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
  manifest.set_version(kManifestVersion);

  auto hash_headers = [&](const List<String>& headers,
                          const List<Literal>& skip_list) {
    for (const auto& header : headers) {
      String error;
      Immutable header_hash;
      const Path header_path =
          Path(header).is_absolute() ? Path(header) : current_dir / header;
      if (!base::File::Hash(header_path, &header_hash, skip_list, &error)) {
        LOG(CACHE_ERROR) << "Failed to hash " << header_path << ": " << error;
        return;
      }
      hash_rope.push_back(header_hash);
      manifest.mutable_direct()->add_headers(header);
    }
  };

  hash_headers(headers, {"__DATE__"_l, "__TIME__"_l});
  hash_headers(preprocessed_headers, {});

  auto direct_hash = base::Hexify(Immutable(hash_rope).Hash());
  DCHECK(database_);
  if (!database_->Set(direct_hash, hash)) {
    return;
  }

  String error;
  if (!base::SaveToFile(manifest_path, manifest, &error)) {
    RemoveEntry(orig_hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({time(nullptr), orig_hash});
}

void FileCache::Clean(UniquePtr<EntryList> list) {
  entries_->BeginTransaction();
  while (auto new_entry = list->Pop()) {
    const auto& hash = new_entry->second;
    SQLite::Value entry;
    if (entries_->Get(hash.str, &entry)) {
      // Update mtime of an existing entry.
      CHECK(entries_->Set(
          hash.str,
          std::make_tuple(new_entry->first, std::get<SQLite::SIZE>(entry),
                          kManifestVersion)));
      LOG(CACHE_VERBOSE) << hash.str << " wasn't used for "
                         << (new_entry->first - std::get<SQLite::MTIME>(entry))
                         << " seconds";
    } else {
      // Insert new entry.
      ui64 size = 0u;
      GetEntrySize(hash, &size);
      CHECK(entries_->Set(
          hash.str, std::make_tuple(new_entry->first, size, kManifestVersion)));
      cache_size_ += size;
      STAT(CACHE_SIZE_ADDED, size);
    }
  }

  if (max_size_ == UNLIMITED) {
    entries_->EndTransaction();
    return;
  }

  while (cache_size_ > max_size_) {
    string::Hash hash;
    SQLite::Value entry;
    CHECK(entries_->First(&hash.str, &entry));

    const auto manifest_path =
        AppendExtension(CommonPath(hash), base::kExtManifest);
    WriteLock lock(this, manifest_path);
    if (lock) {
      LOG(CACHE_VERBOSE) << "Cache overuse is " << (cache_size_ - max_size_)
                         << " bytes: removing " << hash.str;
      DCHECK_O_EVAL(RemoveEntry(hash));
    }
  }
  entries_->EndTransaction();
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
