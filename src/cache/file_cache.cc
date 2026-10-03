#include <cache/file_cache.h>

#include <base/constants.h>
#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/path_utils.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <cache/compression.h>
#include <perf/stat_service.h>

#include STL(map)
#include STL(regex)
#include STL(set)

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

// Bump to invalidate all direct cache entries, e.g. when the way of collecting
// the direct-mode command line changes.
const Literal kDirectKeyVersion = "direct-2"_l;

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

namespace {

const char kRootPlaceholder[] = "<ROOT>";

// The path of the header in the manifest: relative to the source root, if the
// header is under it.
String ToManifestPath(const String& header, const Path& current_dir,
                      const String& root) {
  if (root.empty()) {
    return header;
  }
  const String path =
      (Path(header).is_absolute() ? Path(header) : current_dir / header)
          .lexically_normal()
          .string();
  if (path.starts_with(root + "/")) {
    return kRootPlaceholder + path.substr(root.size());
  }
  return header;
}

// The path of the header to read.
Path FromManifestPath(const String& header, const Path& current_dir,
                      const String& root) {
  if (header.starts_with(kRootPlaceholder)) {
    // Can't be resolved without the root - the empty path is never read.
    return root.empty()
               ? Path()
               : Path(root + header.substr(strlen(kRootPlaceholder)));
  }
  return Path(header).is_absolute() ? Path(header) : current_dir / header;
}

// The compression of the object and split DWARF files of the entry.
proto::Compression GetCompression(const proto::Simple_Version1& v1) {
  if (v1.has_compression()) {
    return v1.compression();
  }
  return v1.snappy() ? proto::SNAPPY : proto::NONE;
}

struct Variant {
  List<String> headers;
  HashSet<String> ignored_paths;
};

List<Variant> DirectVariants(const proto::Direct& direct) {
  List<Variant> variants;
  for (const auto& variant : direct.variants()) {
    variants.push_back(
        {{variant.headers().begin(), variant.headers().end()},
         {variant.ignored_paths().begin(), variant.ignored_paths().end()}});
  }
  if (variants.empty() && direct.headers_size()) {
    variants.push_back({{direct.headers().begin(), direct.headers().end()}, {}});
  }
  return variants;
}

// Only regular files and links to them are found by the compiler.
bool IsFile(const Path& path, struct stat* st) {
  return stat(path.c_str(), st) == 0 && !S_ISDIR(st->st_mode);
}

}  // namespace

FileCache::FileCache(const Path& path, ui64 size,
                     proto::Compression compression, bool store_index,
                     bool link_outputs)
    : path_(ReplaceTildeInPath(path)),
      store_index_(store_index),
      link_outputs_(link_outputs),
      blobs_(path_ / "blobs", compression, link_outputs),
      max_size_(size) {}

FileCache::FileCache(const Path& path)
    : FileCache(path, UNLIMITED, proto::NONE, false, false) {}

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

  database_.reset(new SQLiteKV(path_, "direct"));
  tag_index_.reset(new TagIndex(path_, "tags"));
  if (store_index_) {
    entries_.reset(new SQLite(path_, "index"));
  } else {
    entries_.reset(new SQLite);
  }

  CHECK(clean_period > 0);

  entries_->BeginTransaction();
  base::WalkDirectory(path_, [this](const String& file_path, ui64 mtime, ui64) {
    // The temporary files are left by crashes: only one daemon uses the cache
    // at the moment, and it doesn't write anything yet.
    if (file_path.ends_with(".tmp")) {
      LOG(CACHE_INFO) << "Removing stale temporary file " << file_path;
      base::File::Delete(file_path);
      return;
    }

    // The files of an entry without the manifest are left by a crash in the
    // middle of storing: nothing refers to them.
    std::regex data_regex(
        "[a-f0-9]{32}-[a-f0-9]{8}-[a-f0-9]{8}\\.(o|d|dwo|stderr)$");
    if (std::regex_search(file_path, data_regex)) {
      const auto manifest_path =
          Path(file_path).replace_extension(".manifest");
      if (!base::File::Exists(manifest_path)) {
        LOG(CACHE_INFO) << "Removing file of incomplete entry " << file_path;
        base::File::Delete(file_path);
      }
      return;
    }

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
      if (!from_index &&
          !entries_->Set(hash.str,
                         std::make_tuple(mtime, size, kManifestVersion, 0u))) {
        LOG(CACHE_ERROR) << "Failed to add to the index " << hash.str;
      }

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

  // Count the references to the blobs before any other work of the cleaner.
  const ui64 start_time = time(nullptr);
  cleaner_.Push([this, start_time] { CountBlobRefs(start_time); });
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
      base::Hexify((version.str + "\n"_l + kDirectKeyVersion).Hash(4)));
}

// static
List<Path> FileCache::ShadowingPaths(const Path& header,
                                     const List<Path>& search_dirs) {
  List<Path> result;
  const String& header_str = header.native();
  for (auto dir = search_dirs.begin(); dir != search_dirs.end(); ++dir) {
    String prefix = dir->native();
    if (!prefix.ends_with('/')) {
      prefix.push_back('/');
    }
    if (header_str.size() <= prefix.size() || !header_str.starts_with(prefix)) {
      continue;
    }

    // The header may be found in this directory by its relative path - then a
    // file with the same relative path in any previous directory is found
    // instead.
    const String relative = header_str.substr(prefix.size());
    for (auto other_dir = search_dirs.begin(); other_dir != dir; ++other_dir) {
      Path path = *other_dir / relative;
      if (path != header) {
        result.push_back(std::move(path));
      }
    }
  }
  return result;
}

bool FileCache::IsShadowed(const List<String>& headers,
                           const HashSet<String>& ignored_paths,
                           const Path& current_dir, const String& root,
                           const List<Path>& search_dirs) const {
  if (search_dirs.empty()) {
    return false;
  }

  for (const auto& header : headers) {
    const Path header_path =
        FromManifestPath(header, current_dir, root).lexically_normal();
    for (const auto& path : ShadowingPaths(header_path, search_dirs)) {
      struct stat st;
      if (IsFile(path, &st) &&
          !ignored_paths.contains(
              ToManifestPath(path.string(), current_dir, root))) {
        LOG(CACHE_INFO) << "Header " << header_path << " is shadowed by "
                        << path;
        return true;
      }
    }
  }

  return false;
}

bool FileCache::Find(UnhandledSource code, const ExtraFiles& extra_files,
                     CommandLine command_line, Version version,
                     const Path& current_dir, Entry* entry,
                     const String& root,
                     const List<Path>& search_dirs) const {
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

  // Try all known sets of headers: the same source may include different
  // headers, e.g. on different branches.
  for (const auto& variant : DirectVariants(manifest.direct())) {
    Immutable::Rope hash_rope = {unhandled_hash};
    bool hashed = true;
    for (const auto& header : variant.headers) {
      String header_hash;
      const Path header_path = FromManifestPath(header, current_dir, root);
      if (header_path.empty() || !file_hashes_.Hash(header_path, &header_hash)) {
        hashed = false;
        break;
      }
      hash_rope.push_back(Immutable(std::move(header_hash)));
    }
    if (!hashed) {
      continue;
    }

    Immutable hash_with_headers = base::Hexify(Immutable(hash_rope).Hash());
    Immutable handled_hash;

    DCHECK(database_);
    if (database_->Get(hash_with_headers, &handled_hash) &&
        !IsShadowed(variant.headers, variant.ignored_paths, current_dir, root,
                    search_dirs) &&
        Find(HandledHash(handled_hash), entry)) {
      utime(manifest_path.c_str(), nullptr);
      new_entries_->Append({ui64(time(nullptr)), unhandled_hash, true});
      return true;
    }
  }

  return false;
}

bool FileCache::FindOutput(const Path& cache_path,
                           proto::Compression compression,
                           const String& expected_hash, Immutable* content,
                           Path* path, ui64* size) const {
  // An entry without the hash is broken, or from an old version.
  if (expected_hash.empty()) {
    LOG(CACHE_WARNING) << "No hash for " << cache_path;
    return false;
  }

  auto Broken = [&cache_path](const char* reason) {
    LOG(CACHE_WARNING) << "Cache file is broken (" << reason
                       << "): " << cache_path;
    // Break the entry, so that it's removed later.
    base::File::Delete(cache_path);
    return false;
  };

  // Link the uncompressed output instead of reading it.
  if (link_outputs_ && compression == proto::NONE && path) {
    struct stat st;
    if (stat(cache_path.c_str(), &st) == -1) {
      return false;
    }

    // The file may be shared through a hard link - and modified in place by
    // someone else. The link may be gone already, so check the contents
    // always: it's cheap compared to the compilation.
    Immutable actual_hash;
    if (!base::File::Hash(cache_path, &actual_hash) ||
        actual_hash.string_copy() != expected_hash) {
      return Broken("wrong hash");
    }

    *path = cache_path;
    *size += st.st_size;
    return true;
  }

  Immutable file_content;
  String error;
  if (!base::File::Read(cache_path, &file_content, &error)) {
    LOG(CACHE_ERROR) << "Failed to read " << cache_path << " : " << error;
    return false;
  }
  *size += file_content.size();

  if (compression == proto::NONE) {
    if (base::Hexify(file_content.Hash()) != expected_hash) {
      return Broken("wrong hash");
    }
    *content = file_content;
    return true;
  }

  String unpacked;
  if (!Unpack(compression, file_content, &unpacked)) {
    return Broken("can't unpack");
  }
  Immutable unpacked_content(std::move(unpacked));
  if (base::Hexify(unpacked_content.Hash()) != expected_hash) {
    return Broken("wrong hash");
  }
  *content = unpacked_content;
  return true;
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

  const auto& v1 = manifest.v1();
  ui64 size = 0;

  if (v1.err() &&
      !FindOutput(AppendExtension(CommonPath(hash), base::kExtStderr),
                  proto::NONE,
                  v1.stderr_hash(), &entry->stderr, nullptr, &size)) {
    return false;
  }

  // The blobs aren't a part of the size of the entry.
  ui64 blobs_size = 0;
  if (v1.obj() &&
      !(v1.blobs()
            ? blobs_.Get(v1.object_hash(), &entry->object,
                         &entry->object_path, &blobs_size)
            : FindOutput(AppendExtension(CommonPath(hash), base::kExtObject),
                         GetCompression(v1), v1.object_hash(), &entry->object,
                         &entry->object_path, &size))) {
    return false;
  }
  entry->object_hash = v1.object_hash();

  if (v1.dep() &&
      !FindOutput(AppendExtension(CommonPath(hash), base::kExtDeps),
                  proto::NONE,
                  v1.deps_hash(), &entry->deps, nullptr, &size)) {
    return false;
  }

  if (v1.dwo() &&
      !(v1.blobs()
            ? blobs_.Get(v1.dwo_hash(), &entry->dwo, &entry->dwo_path,
                         &blobs_size)
            : FindOutput(AppendExtension(CommonPath(hash), base::kExtDwo),
                         GetCompression(v1), v1.dwo_hash(), &entry->dwo,
                         &entry->dwo_path, &size))) {
    return false;
  }
  entry->dwo_hash = v1.dwo_hash();
  entry->warnings_hash = v1.warnings_hash();
  entry->handled_hash = hash.str.string_copy();

  if (!v1.has_size() || v1.size() != size) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({ui64(time(nullptr)), hash, true});
  return true;
}

void FileCache::Store(UnhandledSource code, const ExtraFiles& extra_files,
                      CommandLine command_line, Version version,
                      const List<String>& headers,
                      const List<String>& preprocessed_headers,
                      const Path& current_dir, string::HandledHash hash,
                      const String& root, const List<Path>& search_dirs,
                      i64 start_time) {
  DoStore(Hash(code, extra_files, command_line, version), headers,
          preprocessed_headers, current_dir, hash, root, search_dirs,
          start_time);
}

void FileCache::AddTags(string::HandledHash hash, const Vector<String>& tags) {
  if (tags.empty()) {
    return;
  }
  new_entries_->Append({ui64(time(nullptr)), hash, true, tags});
}

void FileCache::ApplyTags(string::Hash hash, const Vector<String>& tags) {
  // The entry may be removed already.
  SQLite::Value value;
  if (!entries_->Get(hash.str, &value)) {
    return;
  }

  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  const String key = hash.str.string_copy();
  auto& entry_tags = entry_tags_[key];
  bool changed = false;
  for (const auto& tag : tags) {
    auto it = std::lower_bound(entry_tags.begin(), entry_tags.end(), tag);
    if (it == entry_tags.end() || *it != tag) {
      entry_tags.insert(it, tag);
      changed = true;
    }
  }
  if (changed) {
    tag_index_->Set(key, entry_tags);
  }
}

bool FileCache::GetTagStats(
    Fn<void(const List<TagStats>&, const List<TagOverlap>&)> callback) {
  return bool(cleaner_.Push([this, callback] {
    List<TagStats> stats;
    List<TagOverlap> overlaps;
    ComputeTagStats(&stats, &overlaps);
    callback(stats, overlaps);
  }));
}

bool FileCache::ForgetTag(const String& tag, bool dry_run,
                          Fn<void(const ForgetResult&)> callback) {
  return bool(cleaner_.Push([this, tag, dry_run, callback] {
    entries_->BeginTransaction();
    tag_index_->BeginTransaction();
    const auto result = DoForgetTag(tag, dry_run);
    tag_index_->EndTransaction();
    entries_->EndTransaction();
    callback(result);
  }));
}

void FileCache::ComputeTagStats(List<TagStats>* stats,
                                List<TagOverlap>* overlaps) {
  const Vector<String> kNoTags = {String()};
  std::lock_guard<Mutex> lock(blob_refs_mutex_);

  // The tags of every blob, and the sizes of the blobs.
  HashMap<String, std::set<String>> blob_tags;
  HashMap<String, ui64> blob_sizes;
  std::map<String, TagStats> tag_stats;
  std::map<Pair<String, String>, ui64> overlap_sizes;

  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    auto tags_it = entry_tags_.find(hash);
    const auto& tags =
        tags_it == entry_tags_.end() || tags_it->second.empty()
            ? kNoTags
            : tags_it->second;
    const ui64 size = std::get<SQLite::SIZE>(value);
    for (const auto& tag : tags) {
      auto& stat = tag_stats[tag];
      stat.tag = tag;
      ++stat.entries;
      stat.size += size;
      if (tags.size() == 1) {
        ++stat.exclusive_entries;
        stat.exclusive_size += size;
      }
    }
    for (auto first = tags.begin(); first != tags.end(); ++first) {
      for (auto second = std::next(first); second != tags.end(); ++second) {
        overlap_sizes[{*first, *second}] += size;
      }
    }

    auto blobs_it = entry_blobs_.find(hash);
    if (blobs_it != entry_blobs_.end()) {
      for (const auto& blob : blobs_it->second) {
        blob_tags[blob].insert(tags.begin(), tags.end());
        if (!blob_sizes.contains(blob)) {
          blob_sizes[blob] = blobs_.Size(blob);
        }
      }
    }
  }

  // Every blob counts once for every tag.
  for (const auto& [blob, tags] : blob_tags) {
    const ui64 size = blob_sizes[blob];
    for (const auto& tag : tags) {
      tag_stats[tag].size += size;
      if (tags.size() == 1) {
        tag_stats[tag].exclusive_size += size;
      }
    }
    for (auto first = tags.begin(); first != tags.end(); ++first) {
      for (auto second = std::next(first); second != tags.end(); ++second) {
        overlap_sizes[{*first, *second}] += size;
      }
    }
  }

  for (auto& [tag, stat] : tag_stats) {
    stats->push_back(std::move(stat));
  }
  for (const auto& [tags, size] : overlap_sizes) {
    overlaps->push_back({tags.first, tags.second, size});
  }
}

FileCache::ForgetResult FileCache::DoForgetTag(const String& tag,
                                               bool dry_run) {
  ForgetResult result;

  // The entries to remove, and how many references of the blobs they have.
  List<String> removed;
  HashMap<String, ui32> removed_refs;
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (auto& [entry, tags] : entry_tags_) {
      if (!std::binary_search(tags.begin(), tags.end(), tag)) {
        continue;
      }
      if (tags.size() > 1) {
        ++result.shared_entries;
        continue;
      }
      removed.push_back(entry);
    }
  }

  for (auto it = removed.begin(); it != removed.end();) {
    const string::Hash hash{Immutable(*it)};
    // The outputs of the current builds are likely to be used again.
    if (GetSharedSize(hash) || HasSharedBlobs(hash)) {
      ++result.linked_entries;
      it = removed.erase(it);
      continue;
    }
    SQLite::Value value;
    if (entries_->Get(*it, &value)) {
      result.freed_size += std::get<SQLite::SIZE>(value);
    }
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    auto blobs_it = entry_blobs_.find(*it);
    if (blobs_it != entry_blobs_.end()) {
      for (const auto& blob : blobs_it->second) {
        ++removed_refs[blob];
      }
    }
    ++it;
  }

  {
    // The blobs are freed, only if no other entry uses them.
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (const auto& [blob, refs] : removed_refs) {
      if (blobs_.Refs(blob) == refs) {
        result.freed_size += blobs_.Size(blob);
      }
    }
  }
  result.removed_entries = removed.size();

  if (dry_run) {
    return result;
  }

  for (const auto& entry : removed) {
    const string::Hash hash{Immutable(entry)};
    WriteLock lock(this, AppendExtension(CommonPath(hash), base::kExtManifest));
    if (lock) {
      RemoveEntry(hash);
    }
  }

  // The other entries keep their other tags.
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  for (auto& [entry, tags] : entry_tags_) {
    auto it = std::lower_bound(tags.begin(), tags.end(), tag);
    if (it != tags.end() && *it == tag && tags.size() > 1) {
      tags.erase(it);
      tag_index_->Set(entry, tags);
    }
  }

  return result;
}

bool FileCache::MarkSeen(const String& key) {
  DCHECK(database_);
  const String db_key = "seen:" + key;
  Immutable value;
  if (database_->Get(db_key, &value)) {
    return false;
  }
  database_->Set(db_key, "1"_l);
  return true;
}

bool FileCache::ReplaceLastSource(const String& key, UnhandledHash hash) {
  DCHECK(database_);
  const String db_key = "source:" + key;
  Immutable previous;
  const bool edited =
      database_->Get(db_key, &previous) && !(previous == hash.str);
  if (!edited && !previous.empty()) {
    return false;
  }
  database_->Set(db_key, hash.str);
  return edited;
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
  auto* v1 = manifest.mutable_v1();
  v1->set_blobs(true);
  ui64 size = 0;

  // The outputs of the old format.
  for (const auto& extension : {base::kExtObject, base::kExtDwo}) {
    const auto path = AppendExtension(CommonPath(hash), extension);
    if (base::File::Exists(path)) {
      base::File::Delete(path);
    }
  }

  v1->set_err(!entry.stderr.empty());
  if (!entry.stderr.empty()) {
    const auto stderr_path =
        AppendExtension(CommonPath(hash), base::kExtStderr);

    if (!base::File::Write(stderr_path, entry.stderr, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save stderr to " << stderr_path << ": "
                       << error;
      return;
    }
    size += entry.stderr.size();
    v1->set_stderr_hash(base::Hexify(entry.stderr.Hash()));
  }

  v1->set_obj(!entry.object.empty() || !entry.object_path.empty());
  bool reused = false;
  if (v1->obj()) {
    if (!blobs_.Put(entry.object, entry.object_path,
                    v1->mutable_object_hash(), &reused, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save object of " << hash.str << ": "
                       << error;
      return;
    }
    if (reused) {
      STAT(CACHE_BLOB_REUSED);
    }
  }

  v1->set_dep(!entry.deps.empty());
  if (!entry.deps.empty()) {
    const auto deps_path = AppendExtension(CommonPath(hash), base::kExtDeps);

    if (!base::File::Write(deps_path, entry.deps, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save deps to " << deps_path << ": "
                       << error;
      return;
    }
    size += entry.deps.size();
    v1->set_deps_hash(base::Hexify(entry.deps.Hash()));
  }

  v1->set_dwo(!entry.dwo.empty() || !entry.dwo_path.empty());
  if (v1->dwo()) {
    if (!blobs_.Put(entry.dwo, entry.dwo_path, v1->mutable_dwo_hash(),
                    &reused, &error)) {
      RemoveEntry(hash);
      LOG(CACHE_ERROR) << "Failed to save split DWARF of " << hash.str << ": "
                       << error;
      return;
    }
    if (reused) {
      STAT(CACHE_BLOB_REUSED);
    }
  }

  v1->set_size(size);
  if (!entry.warnings_hash.empty()) {
    v1->set_warnings_hash(entry.warnings_hash);
  }

  if (!base::SaveToFile(manifest_path, manifest, &error)) {
    RemoveEntry(hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({ui64(time(nullptr)), hash, false});

  LOG(CACHE_VERBOSE) << "File is cached on path " << CommonPath(hash);
}

void FileCache::Remove(string::HandledHash hash) {
  // The index is updated only by the cleaner.
  cleaner_.Push([this, hash] {
    const auto manifest_path =
        AppendExtension(CommonPath(hash), base::kExtManifest);
    WriteLock lock(this, manifest_path);
    if (!lock || !base::File::Exists(manifest_path)) {
      return;
    }
    entries_->BeginTransaction();
    RemoveEntry(hash);
    entries_->EndTransaction();
  });
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
  const String dwo_path = common_path + ".dwo";
  bool result = true;
  auto entry_size = has_entry ? std::get<SQLite::SIZE>(entry) : 0u;

  if (has_entry) {
    entries_->Delete(hash.str);
  } else {
    LOG(CACHE_WARNING) << "Removing unconsidered entry: " << hash.str;
  }
  RemoveBlobRefs(hash);
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    if (entry_tags_.erase(hash.str.string_copy())) {
      tag_index_->Delete(hash.str.string_copy());
    }
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

  if (base::File::Exists(dwo_path)) {
    if (!base::File::Delete(dwo_path, &error)) {
      entry_size -= base::File::Size(dwo_path);
      result = false;
      LOG(CACHE_WARNING) << "Failed to delete " << dwo_path << ": " << error;
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
                        const Path& current_dir, const HandledHash& hash,
                        const String& root, const List<Path>& search_dirs,
                        i64 start_time) {
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

  // Keep the previous variants of headers.
  proto::Manifest old_manifest;
  List<Variant> variants;
  if (base::File::Exists(manifest_path) &&
      base::LoadFromFile(manifest_path, &old_manifest) &&
      old_manifest.has_direct()) {
    variants = DirectVariants(old_manifest.direct());
  }

  proto::Manifest manifest;
  manifest.set_version(kManifestVersion);
  Variant new_variant;

  auto hash_headers = [&](const List<String>& headers,
                          const List<Literal>& skip_list) {
    for (const auto& header : headers) {
      String error;
      Immutable header_hash;
      const Path header_path =
          Path(header).is_absolute() ? Path(header) : current_dir / header;
      // Precompiled headers are binary - and already contain the expanded
      // time macros, if any.
      const auto extension = header_path.extension();
      const bool is_precompiled = extension == ".pch" || extension == ".gch" ||
                                  extension == ".pcm";
      String precompiled_hash;
      if (is_precompiled &&
          file_hashes_.Hash(header_path, &precompiled_hash, &error)) {
        header_hash = Immutable(std::move(precompiled_hash));
      } else if (is_precompiled ||
                 !base::File::Hash(header_path, &header_hash, skip_list,
                                   &error)) {
        LOG(CACHE_INFO) << "Not storing direct cache entry - failed to hash "
                        << header_path << ": " << error;
        return false;
      }
      hash_rope.push_back(header_hash);
      new_variant.headers.push_back(ToManifestPath(header, current_dir, root));
    }
    return true;
  };

  // The files, that could be found instead of the headers, but weren't, are
  // remembered - the others must not appear later.
  auto collect_ignored_paths = [&](const List<String>& headers) {
    for (const auto& header : headers) {
      const Path header_path =
          (Path(header).is_absolute() ? Path(header) : current_dir / header)
              .lexically_normal();
      for (const auto& path : FileCache::ShadowingPaths(header_path,
                                                        search_dirs)) {
        struct stat st;
        if (!IsFile(path, &st)) {
          continue;
        }
        // The compiler may have missed the file, that is created during the
        // compilation.
        const i64 ctime =
            i64(st.st_ctim.tv_sec) * 1'000'000'000 + st.st_ctim.tv_nsec;
        if (start_time && ctime > start_time) {
          LOG(CACHE_INFO) << "Not storing direct cache entry - " << path
                          << " is created during the compilation";
          return false;
        }
        new_variant.ignored_paths.insert(
            ToManifestPath(path.string(), current_dir, root));
      }
    }
    return true;
  };

  // The result depends on the time, if these macros are used - the direct
  // cache can't detect it. The simple cache is still fine, since the macros
  // are expanded in the preprocessed source.
  const List<Literal> time_macros = {"__DATE__"_l, "__TIME__"_l,
                                     "__TIMESTAMP__"_l};
  if (!hash_headers(headers, time_macros) ||
      !hash_headers(preprocessed_headers, {}) ||
      !collect_ignored_paths(headers) ||
      !collect_ignored_paths(preprocessed_headers)) {
    return;
  }

  auto direct_hash = base::Hexify(Immutable(hash_rope).Hash());
  DCHECK(database_);
  if (!database_->Set(direct_hash, hash)) {
    return;
  }

  // The most recent variant goes first, the oldest ones are dropped.
  variants.remove_if([&new_variant](const Variant& variant) {
    return variant.headers == new_variant.headers;
  });
  variants.push_front(std::move(new_variant));
  while (variants.size() > kMaxDirectVariants) {
    variants.pop_back();
  }
  for (const auto& variant : variants) {
    auto* new_variant = manifest.mutable_direct()->add_variants();
    for (const auto& header : variant.headers) {
      new_variant->add_headers(header);
    }
    // Sorted - for the stable contents of the manifest.
    Vector<String> ignored_paths(variant.ignored_paths.begin(),
                                 variant.ignored_paths.end());
    std::sort(ignored_paths.begin(), ignored_paths.end());
    for (const auto& path : ignored_paths) {
      new_variant->add_ignored_paths(path);
    }
  }

  String error;
  if (!base::SaveToFile(manifest_path, manifest, &error)) {
    RemoveEntry(orig_hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({ui64(time(nullptr)), orig_hash, false});
}

void FileCache::Clean(UniquePtr<EntryList> list) {
  entries_->BeginTransaction();
  tag_index_->BeginTransaction();
  while (auto access = list->Pop()) {
    const auto& hash = access->hash;
    if (!access->tags.empty()) {
      ApplyTags(hash, access->tags);
      continue;
    }
    SQLite::Value entry;
    if (entries_->Get(hash.str, &entry) && access->hit) {
      // Update the access time and the hits of an existing entry.
      std::get<SQLite::MTIME>(entry) = access->time;
      std::get<SQLite::HITS>(entry) += 1;
      if (!entries_->Set(hash.str, entry)) {
        LOG(CACHE_ERROR) << "Failed to update the index for " << hash.str;
      }
      std::lock_guard<Mutex> lock(blob_refs_mutex_);
      auto it = entry_blobs_.find(hash.str.string_copy());
      if (it != entry_blobs_.end()) {
        for (const auto& blob : it->second) {
          blobs_.Touch(blob, access->time);
        }
      }
    } else {
      AddBlobRefs(hash, access->time);
      // Insert a new entry, or replace the stored again one.
      const bool exists = entries_->Get(hash.str, &entry);
      ui64 size = 0u;
      GetEntrySize(hash, &size);
      if (exists) {
        cache_size_ -= std::get<SQLite::SIZE>(entry);
      }
      if (!entries_->Set(
          hash.str,
          std::make_tuple(access->time, size, kManifestVersion,
                          exists ? std::get<SQLite::HITS>(entry) : 0u))) {
        LOG(CACHE_ERROR) << "Failed to update the index for " << hash.str;
      }
      cache_size_ += size;
      STAT(CACHE_SIZE_ADDED, size);
    }
  }

  Evict();
  CompressColdEntries();
  tag_index_->EndTransaction();
  entries_->EndTransaction();
}

ui32 FileCache::CompressColdEntries() {
  if (!compress_after_) {
    return 0;
  }

  // Don't hold the cleaner for too long.
  const ui64 kMaxCompressedSizePerPass = 1ull << 30;
  const ui64 now = time(nullptr);
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  return blobs_.CompressUnused(now - std::min(now, compress_after_),
                               kMaxCompressedSizePerPass);
}

void FileCache::AddBlobRefs(string::Hash hash, ui64 access_time) {
  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  proto::Manifest manifest;
  Vector<String> blobs;
  if (base::LoadFromFile(manifest_path, &manifest) && manifest.has_v1() &&
      manifest.v1().blobs()) {
    if (manifest.v1().obj()) {
      blobs.push_back(manifest.v1().object_hash());
    }
    if (manifest.v1().dwo()) {
      blobs.push_back(manifest.v1().dwo_hash());
    }
  }

  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  // The new references go first: the blobs may be the same.
  for (const auto& blob : blobs) {
    blobs_.AddRef(blob, access_time);
  }
  const String key = hash.str.string_copy();
  auto it = entry_blobs_.find(key);
  if (it != entry_blobs_.end()) {
    for (const auto& blob : it->second) {
      blobs_.RemoveRef(blob);
    }
    entry_blobs_.erase(it);
  }
  if (!blobs.empty()) {
    entry_blobs_.emplace(key, std::move(blobs));
  }
}

void FileCache::RemoveBlobRefs(string::Hash hash) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  auto it = entry_blobs_.find(hash.str.string_copy());
  if (it != entry_blobs_.end()) {
    for (const auto& blob : it->second) {
      blobs_.RemoveRef(blob);
    }
    entry_blobs_.erase(it);
  }
}

void FileCache::CountBlobRefs(ui64 start_time) {
  for (const auto& [hash, entry] : entries_->GetInEvictionOrder()) {
    AddBlobRefs(string::Hash(Immutable(hash)), std::get<SQLite::MTIME>(entry));
  }

  // The tags of the removed entries may stay after a crash.
  {
    auto all_tags = tag_index_->GetAll();
    tag_index_->BeginTransaction();
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (auto& [entry, tags] : all_tags) {
      SQLite::Value value;
      if (entries_->Get(entry, &value)) {
        entry_tags_[entry] = std::move(tags);
      } else {
        tag_index_->Delete(entry);
      }
    }
    tag_index_->EndTransaction();
  }

  // E.g. the blobs of the entries, that weren't stored because of a crash.
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  blobs_.RemoveUnreferenced(start_time);
}

bool FileCache::HasSharedBlobs(string::Hash hash) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  auto it = entry_blobs_.find(hash.str.string_copy());
  if (it == entry_blobs_.end()) {
    return false;
  }
  for (const auto& blob : it->second) {
    if (blobs_.IsShared(blob)) {
      return true;
    }
  }
  return false;
}

ui64 FileCache::GetOwnBlobsSize(string::Hash hash) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  auto it = entry_blobs_.find(hash.str.string_copy());
  if (it == entry_blobs_.end()) {
    return 0;
  }
  ui64 size = 0;
  for (const auto& blob : it->second) {
    if (blobs_.Refs(blob) == 1 && !blobs_.IsShared(blob)) {
      size += blobs_.Size(blob);
    }
  }
  return size;
}

ui64 FileCache::GetSharedSize(string::Hash hash) const {
  if (!link_outputs_) {
    return 0;
  }

  ui64 shared_size = 0;
  for (const auto& extension : {base::kExtObject, base::kExtDwo}) {
    struct stat st;
    const auto path = AppendExtension(CommonPath(hash), extension);
    if (stat(path.c_str(), &st) == 0 && st.st_nlink > 1) {
      shared_size += st.st_size;
    }
  }
  return shared_size;
}

void FileCache::Evict() {
  if (max_size_ == UNLIMITED && !min_free_percent_) {
    return;
  }

  // The entries, which outputs are shared with build directories, are free:
  // their removal doesn't free the disk space. They are likely to be used
  // soon, so they are never removed.
  auto entries = entries_->GetInEvictionOrder();
  ui64 own_size = 0, shared_size = 0;
  Vector<ui64> own_sizes;
  own_sizes.reserve(entries.size());
  for (const auto& [hash_str, entry] : entries) {
    const string::Hash hash{Immutable(hash_str)};
    const ui64 shared = GetSharedSize(hash);
    const ui64 total = std::get<SQLite::SIZE>(entry);
    own_sizes.push_back(shared || HasSharedBlobs(hash) ? 0 : total);
    own_size += total - std::min(total, shared);
    shared_size += std::min(total, shared);
  }
  {
    // Every blob counts once.
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    blobs_.GetTotalSize(&own_size, &shared_size);
  }

  base::Singleton<perf::StatService>::Get().Set(
      perf::proto::Metric::CACHE_OWN_SIZE, own_size);
  base::Singleton<perf::StatService>::Get().Set(
      perf::proto::Metric::CACHE_SHARED_SIZE, shared_size);
  base::Singleton<perf::StatService>::Get().Set(
      perf::proto::Metric::CACHE_ENTRIES, entries.size());

  // Clean a bit more than necessary - to clean less often.
  ui64 target_size = max_size_;
  if (max_size_ != UNLIMITED && own_size > max_size_) {
    target_size = max_size_ - max_size_ / 10;
  }

  // Keep some free space on the disk, since the build directories grow too.
  ui64 need_to_free = 0;
  if (min_free_percent_) {
    struct statvfs st;
    if (statvfs(path_.c_str(), &st) == 0) {
      const ui64 total = ui64(st.f_blocks) * st.f_frsize;
      const ui64 free = ui64(st.f_bavail) * st.f_frsize;
      const ui64 min_free = total / 100 * min_free_percent_;
      if (free < min_free) {
        need_to_free = min_free - free;
      }
    }
  }

  ui64 freed = 0;
  auto own_size_it = own_sizes.begin();
  for (const auto& [hash_str, entry] : entries) {
    const ui64 entry_own_size = *(own_size_it++);
    if ((max_size_ == UNLIMITED || own_size <= target_size) &&
        freed >= need_to_free) {
      break;
    }
    if (!entry_own_size) {
      continue;
    }

    const string::Hash hash{Immutable(hash_str)};
    const auto manifest_path =
        AppendExtension(CommonPath(hash), base::kExtManifest);
    WriteLock lock(this, manifest_path);
    if (lock) {
      LOG(CACHE_VERBOSE) << "Removing " << hash.str << " with "
                         << std::get<SQLite::HITS>(entry) << " hits";
      // The blobs, that are used by other entries, stay.
      const ui64 removed_size = entry_own_size + GetOwnBlobsSize(hash);
      RemoveEntry(hash);
      own_size -= std::min(own_size, removed_size);
      freed += removed_size;
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
