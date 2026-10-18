// The macro is undefined by the headers of the index.
static const char kDaemonVersion[] = VERSION;

#include <cache/file_cache.h>

#include <base/constants.h>
#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/path_utils.h>
#include <base/platform.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <cache/compression.h>
#include <perf/stat_service.h>

#include STL(future)
#include STL(map)
#include STL(set)
#include STL(thread)

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

// Bump to invalidate all direct cache entries, e.g. when the way of collecting
// the direct-mode command line changes.
const Literal kDirectKeyVersion = "direct-4"_l;

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
  return variants;
}

// The blobs of the entry in the index.
String JoinBlobs(const Vector<String>& blobs) {
  String result;
  for (const auto& blob : blobs) {
    result += (result.empty() ? "" : "\n") + blob;
  }
  return result;
}

Vector<String> SplitBlobs(const String& blobs) {
  List<String> list;
  base::SplitString<'\n'>(blobs, list);
  return Vector<String>(list.begin(), list.end());
}

// Only regular files and links to them are found by the compiler.
bool IsFile(const Path& path, struct stat* st) {
  return stat(path.c_str(), st) == 0 && !S_ISDIR(st->st_mode);
}

// The search directories with the trailing separators.
Vector<String> SearchPrefixes(const List<Path>& search_dirs) {
  Vector<String> prefixes;
  for (const auto& dir : search_dirs) {
    String prefix = dir.native();
    if (!prefix.ends_with('/')) {
      prefix.push_back('/');
    }
    prefixes.push_back(std::move(prefix));
  }
  return prefixes;
}

// Calls |visit(prefix, relative)| for the paths, where a new file would be
// found instead of |header| - see |FileCache::ShadowingPaths()|.
template <class Visit>
void VisitShadowingPaths(const String& header, const Vector<String>& prefixes,
                         Visit visit) {
  for (size_t dir = 0; dir < prefixes.size(); ++dir) {
    const String& prefix = prefixes[dir];
    if (header.size() <= prefix.size() ||
        header.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    // The header may be found in this directory by its relative path - then a
    // file with the same relative path in any previous directory is found
    // instead.
    const std::string_view relative(header.c_str() + prefix.size(),
                                    header.size() - prefix.size());
    for (size_t other_dir = 0; other_dir < dir; ++other_dir) {
      visit(prefixes[other_dir], relative);
    }
  }
}

// Checks the files, that could shadow the headers. Most of them are in the
// directories, that don't exist: each directory on the way to a file is
// checked once - without building the paths of all candidates.
class ShadowChecker {
 public:
  // Returns the path of the file |prefix| + |relative|, if it exists.
  bool IsFile(const String& prefix, std::string_view relative, String* path,
              struct stat* st) {
    path->assign(prefix);
    size_t start = 0, end;
    while ((end = relative.find('/', start)) != std::string_view::npos) {
      path->append(relative.substr(start, end - start));
      if (!IsDirectory(*path)) {
        return false;
      }
      path->push_back('/');
      start = end + 1;
    }
    path->append(relative.substr(start));
    return cache::IsFile(*path, st);
  }

 private:
  bool IsDirectory(const String& path) {
    auto it = dirs_.find(path);
    if (it == dirs_.end()) {
      struct stat st;
      it = dirs_.emplace(path, stat(path.c_str(), &st) == 0 &&
                                   S_ISDIR(st.st_mode))
               .first;
    }
    return it->second;
  }

  HashMap<String, bool> dirs_;
};

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

namespace {

const char kFormatFile[] = "format";
// Exists, while the daemon uses the cache: after a crash it's left.
const char kRunningFile[] = "running";
const char kRemovedSuffix[] = ".removed-";
// The sizes check all blobs - the cleaner spends on it not more than 1% of the
// time.
const ui32 kSizesReportCost = 100;

// The files and the directories in the root of the cache.
bool IsCacheFile(const String& name) {
  if (name.size() == 1 && isxdigit(name[0])) {
    return true;
  }
  for (const char* prefix :
       {"index.sqlite", "direct.sqlite", "tags.sqlite", "ghosts.sqlite"}) {
    if (name.starts_with(prefix)) {
      return true;
    }
  }
  return name == "blobs" || name == kFormatFile || name == kRunningFile;
}

// The path without the trailing separator - to put another one beside it.
Path WithoutTrailingSeparator(const Path& path) {
  Path result = path.lexically_normal();
  return result.has_filename() ? result : result.parent_path();
}

// Removes the caches of the older generations, that are moved aside, in
// background: they may be large.
void RemoveOldCaches(const Path& cache_path) {
  const Path path = WithoutTrailingSeparator(cache_path);
  std::error_code ec;
  List<Path> old_caches;
  const String prefix = path.filename().string() + kRemovedSuffix;
  for (const auto& entry :
       std::filesystem::directory_iterator(path.parent_path(), ec)) {
    if (entry.path().filename().string().starts_with(prefix)) {
      old_caches.push_back(entry.path());
    }
  }
  if (old_caches.empty()) {
    return;
  }
  std::thread([old_caches] {
    for (const auto& old_cache : old_caches) {
      std::error_code ec;
      std::filesystem::remove_all(old_cache, ec);
      LOG(CACHE_INFO) << "Removed the old cache " << old_cache;
    }
  }).detach();
}

}  // namespace

FileCache::RunningMarker::~RunningMarker() {
  if (!path.empty()) {
    base::File::Delete(path);
  }
}

bool FileCache::CheckFormat() {
  const Path format_path = path_ / kFormatFile;
  proto::Format format;

  if (base::File::Exists(format_path)) {
    if (!base::LoadFromFile(format_path, &format)) {
      // Can't tell anything - start anew.
      format.set_generation(0);
    }
  } else if (base::File::Exists(path_ / "index.sqlite") ||
             base::File::Exists(path_ / "direct.sqlite")) {
    // The cache before the generations.
    format.set_generation(1);
    format.set_revision(0);
  } else {
    format.set_generation(kGeneration);
    format.set_revision(0);
  }

  if (format.generation() > kGeneration) {
    LOG(CACHE_ERROR) << "The cache " << path_ << " has the generation "
                     << format.generation() << " - written by "
                     << format.written_by()
                     << ", this daemon supports only the generation "
                     << kGeneration << ": the cache isn't used";
    base::Singleton<perf::StatService>::Get().Set(
        perf::proto::Metric::CACHE_INCOMPATIBLE, 1);
    return false;
  }

  if (format.generation() < kGeneration) {
    LOG(CACHE_WARNING) << "The cache " << path_ << " has the old generation "
                       << format.generation() << " - it's removed";
    // Move the files of the cache aside at once, and remove them in
    // background. Only the files of the cache: the directory may have others.
    const Path removed_path = WithoutTrailingSeparator(path_).string() +
                              kRemovedSuffix + std::to_string(time(nullptr)) +
                              "-" + std::to_string(getpid());
    String error;
    std::error_code ec;
    const bool moved = base::CreateDirectory(removed_path, &error);
    for (const auto& entry : std::filesystem::directory_iterator(path_, ec)) {
      const String name = entry.path().filename().string();
      if (!IsCacheFile(name)) {
        continue;
      }
      if (!moved || !base::File::Move(entry.path(), removed_path / name)) {
        std::filesystem::remove_all(entry.path(), ec);
      }
    }
    format.Clear();
    format.set_generation(kGeneration);
    format.set_revision(0);
  }

  // The newer revisions of the same generation stay: their daemons may still
  // be used.
  if (format.revision() < kRevision) {
    format.set_revision(kRevision);
    format.set_written_by(kDaemonVersion);
    String error;
    if (!base::SaveToFile(format_path, format, &error)) {
      LOG(CACHE_ERROR) << "Failed to save the format of the cache: " << error;
      return false;
    }
  }

  return true;
}

ui64 FileCache::MetadataSize() const {
  return database_->SizeOnDisk() + tag_index_->SizeOnDisk() +
         ghosts_->SizeOnDisk() +
         base::File::Size(path_ / kFormatFile) +
         base::File::Size(path_ / kRunningFile);
}

namespace {

// Checks the name of the files of the entries: "<32 hex>-<8 hex>-<8 hex>".
bool IsEntryHash(const String& name) {
  if (name.size() != 50 || name[32] != '-' || name[41] != '-') {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    if (i != 32 && i != 41 && !isxdigit(name[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

void FileCache::WaitForCleaner() {
  std::promise<void> done;
  if (cleaner_.Push([&done] { done.set_value(); })) {
    done.get_future().wait();
  }
}

void FileCache::Close() {
  resetter_.reset();
  new_entries_.reset(new EntryList, new_entries_deleter_);
  WaitForCleaner();
  if (!running_marker_.path.empty()) {
    base::File::Delete(running_marker_.path);
    running_marker_.path.clear();
  }
}

void FileCache::ScanEntries(ui64 start_time) {
  entries_->BeginTransaction();
  base::WalkDirectory(path_, [this, start_time](const String& file_path,
                                                ui64 mtime, ui64) {
    // The temporary files are left by crashes - but the new ones are written
    // right now.
    if (file_path.ends_with(".tmp")) {
      if (mtime < start_time) {
        LOG(CACHE_INFO) << "Removing stale temporary file " << file_path;
        base::File::Delete(file_path);
      }
      return;
    }

    const Path path(file_path);
    const String extension = path.extension().string();
    const String hash_str = path.stem().string();
    if (!IsEntryHash(hash_str)) {
      return;
    }

    // The files of an entry without the manifest are left by a crash in the
    // middle of storing: nothing refers to them. The new entries are being
    // stored right now.
    if (extension == ".d" || extension == ".stderr") {
      if (mtime < start_time &&
          !base::File::Exists(Path(file_path).replace_extension(".manifest"))) {
        LOG(CACHE_INFO) << "Removing file of incomplete entry " << file_path;
        base::File::Delete(file_path);
      }
      return;
    }
    if (extension != ".manifest") {
      return;
    }

    // The daemon serves the compilations already: the new entries are added
    // by the cleaner anyway, and the indexed ones need nothing.
    if (mtime >= start_time) {
      return;
    }
    const string::Hash hash{Immutable(hash_str)};
    SQLite::Value value;
    if (entries_->Get(hash.str, &value) && std::get<SQLite::SIZE>(value)) {
      return;
    }
    WriteLock lock(this, file_path);
    if (!lock) {
      return;
    }

    // The entries of other versions aren't migrated - just removed.
    EntryInfo info;
    if (!LoadEntryInfo(hash, &info)) {
      RemoveEntry(hash);
      return;
    }
    if (!entries_->Set(hash.str,
                       std::make_tuple(mtime, info.size, kManifestVersion, 0u,
                                       info.direct, JoinBlobs(info.blobs)))) {
      LOG(CACHE_ERROR) << "Failed to add to the index " << hash.str;
    }
  });

  // The entries, that are gone, but still in the index.
  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    if (std::get<SQLite::MTIME>(value) < start_time &&
        !base::File::Exists(AppendExtension(
            CommonPath(string::Hash(Immutable(hash))), base::kExtManifest))) {
      entries_->Delete(hash);
    }
  }
  entries_->EndTransaction();
}

bool FileCache::Run(ui64 clean_period) {
  String error;
  if (!base::CreateDirectory(path_, &error)) {
    LOG(CACHE_ERROR) << "Failed to create directory " << path_ << " : "
                     << error;
    return false;
  }

  if (!CheckFormat()) {
    return false;
  }
  RemoveOldCaches(path_);

  database_.reset(new SQLiteKV(path_, "direct"));
  tag_index_.reset(new TagIndex(path_, "tags"));
  ghosts_.reset(new GhostIndex(path_, "ghosts"));
  if (store_index_) {
    entries_.reset(new SQLite(path_, "index"));
  } else {
    entries_.reset(new SQLite);
  }

  // The files are checked only after a crash, or with a new index: otherwise
  // the index has all entries.
  const Path running_path = path_ / kRunningFile;
  const bool check_files = entries_->created() ||
                           base::File::Exists(running_path);
  if (!base::File::Write(running_path, Immutable(std::to_string(getpid())),
                         &error)) {
    LOG(CACHE_ERROR) << "Failed to write " << running_path << ": " << error;
    return false;
  }
  running_marker_.path = running_path;

  CHECK(clean_period > 0);

  new_entries_.reset(new EntryList, new_entries_deleter_);

  const ui64 flush_period = std::min(clean_period, flush_period_);
  base::WorkerPool::SimpleWorker worker =
      [this, clean_period, flush_period](const base::WorkerPool& pool) {
        ui64 elapsed = 0;
        while (!pool.WaitUntilShutdown(Seconds(flush_period))) {
          new_entries_.reset(new EntryList, new_entries_deleter_);
          elapsed += flush_period;
          if (elapsed >= clean_period) {
            elapsed = 0;
            cleaner_.Push([this] { Maintain(); });
          }
        }
      };
  resetter_->AddWorker("Cache Resetter Worker"_l, worker);

  // The cleaner works in background - it shouldn't slow down the
  // compilations.
  cleaner_.Push([] { base::LowerCurrentThreadPriority(); });

  // Check the files of the cache, and count the references to the blobs
  // before any other work of the cleaner - in background: the daemon serves
  // the compilations at once.
  const ui64 start_time = time(nullptr);
  cleaner_.Push([this, start_time, check_files] {
    if (check_files) {
      LOG(CACHE_INFO) << "Checking the files of the cache " << path_;
      ScanEntries(start_time);
    }
    CountBlobRefs(start_time);
  });
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
  VisitShadowingPaths(header.native(), SearchPrefixes(search_dirs),
                      [&](const String& prefix, std::string_view relative) {
                        result.emplace_back(prefix + String(relative));
                      });
  return result;
}

bool FileCache::IsShadowed(const List<String>& headers,
                           const HashSet<String>& ignored_paths,
                           const Path& current_dir, const String& root,
                           const List<Path>& search_dirs) const {
  if (search_dirs.empty()) {
    return false;
  }

  ShadowChecker checker;
  const auto prefixes = SearchPrefixes(search_dirs);
  bool shadowed = false;
  String path;
  for (const auto& header : headers) {
    const String header_path =
        FromManifestPath(header, current_dir, root).lexically_normal().native();
    VisitShadowingPaths(
        header_path, prefixes,
        [&](const String& prefix, std::string_view relative) {
          struct stat st;
          if (!shadowed && checker.IsFile(prefix, relative, &path, &st) &&
              !ignored_paths.contains(
                  ToManifestPath(path, current_dir, root))) {
            LOG(CACHE_INFO) << "Header " << header_path << " is shadowed by "
                            << path;
            shadowed = true;
          }
        });
    if (shadowed) {
      return true;
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
  if (!base::LoadFromFile(manifest_path, &manifest) || !manifest.has_direct() ||
      manifest.version() != kManifestVersion) {
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
      // The paths are a part of the key: the same contents in another place
      // give another object and another dependency file.
      hash_rope.push_back(Immutable(header));
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

bool FileCache::FindFile(const Path& cache_path, const String& expected_hash,
                         Immutable* content, ui64* size) const {
  // An entry without the hash is broken.
  if (expected_hash.empty()) {
    LOG(CACHE_WARNING) << "No hash for " << cache_path;
    return false;
  }

  Immutable file_content;
  String error;
  if (!base::File::Read(cache_path, &file_content, &error)) {
    LOG(CACHE_ERROR) << "Failed to read " << cache_path << " : " << error;
    return false;
  }
  *size += file_content.size();

  if (base::Hexify(file_content.Hash()) != expected_hash) {
    LOG(CACHE_WARNING) << "Cache file is broken (wrong hash): " << cache_path;
    // Break the entry, so that it's removed later.
    base::File::Delete(cache_path);
    return false;
  }
  *content = file_content;
  return true;
}

bool FileCache::Find(HandledHash hash, Entry* found) const {
  DCHECK(found);

  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest) || !manifest.has_v1() ||
      manifest.version() != kManifestVersion) {
    return false;
  }

  const auto& v1 = manifest.v1();
  ui64 size = 0;
  // The fields of the previous lookups mustn't stay: e.g. the compressed
  // object comes without a path. The entry is set only on success.
  Entry result;
  Entry* entry = &result;

  if (v1.err() &&
      !FindFile(AppendExtension(CommonPath(hash), base::kExtStderr),
                v1.stderr_hash(), &entry->stderr, &size)) {
    return false;
  }

  // The blobs aren't a part of the size of the entry.
  ui64 blobs_size = 0;
  if (v1.obj() && !blobs_.Get(v1.object_hash(), &entry->object,
                              &entry->object_path, &blobs_size)) {
    return false;
  }
  entry->object_hash = v1.object_hash();

  if (v1.dep() && !FindFile(AppendExtension(CommonPath(hash), base::kExtDeps),
                            v1.deps_hash(), &entry->deps, &size)) {
    return false;
  }

  if (v1.dwo() &&
      !blobs_.Get(v1.dwo_hash(), &entry->dwo, &entry->dwo_path, &blobs_size)) {
    return false;
  }
  entry->dwo_hash = v1.dwo_hash();
  entry->warnings_hash = v1.warnings_hash();
  entry->compilation_time = v1.compilation_time();
  entry->handled_hash = hash.str.string_copy();

  if (!v1.has_size() || v1.size() != size) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  new_entries_->Append({ui64(time(nullptr)), hash, true});
  // The strings of the entry are assigned only once - replace it whole.
  std::destroy_at(found);
  std::construct_at(found, std::move(result));
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

bool FileCache::GetTagStats(TagStatsCallback callback, NeededFn needed) {
  {
    // Join the waiting request.
    std::lock_guard<Mutex> lock(tag_stats_mutex_);
    const bool waiting = !tag_stats_requests_.empty();
    tag_stats_requests_.emplace_back(std::move(callback), std::move(needed));
    if (waiting) {
      return true;
    }
  }

  // The recent accesses go first.
  new_entries_.reset(new EntryList, new_entries_deleter_);
  return bool(cleaner_.Push([this] {
    List<Pair<TagStatsCallback, NeededFn>> requests;
    {
      std::lock_guard<Mutex> lock(tag_stats_mutex_);
      requests.swap(tag_stats_requests_);
    }
    requests.remove_if([](const auto& request) { return !request.second(); });
    if (requests.empty()) {
      return;
    }

    List<TagStats> stats;
    List<TagOverlap> overlaps;
    ComputeTagStats(&stats, &overlaps);
    for (const auto& request : requests) {
      request.first(stats, overlaps);
    }
  }));
}

bool FileCache::ForgetTag(const String& tag, bool dry_run,
                          Fn<void(const ForgetResult&)> callback,
                          NeededFn needed) {
  new_entries_.reset(new EntryList, new_entries_deleter_);
  return bool(cleaner_.Push([this, tag, dry_run, callback, needed] {
    // E.g. the user has interrupted it.
    if (!needed()) {
      return;
    }
    entries_->BeginTransaction();
    tag_index_->BeginTransaction();
    ghosts_->BeginTransaction();
    const auto result = DoForgetTag(tag, dry_run);
    ghosts_->EndTransaction();
    tag_index_->EndTransaction();
    entries_->EndTransaction();
    callback(result);
  }));
}

void FileCache::ComputeTagStats(List<TagStats>* stats,
                                List<TagOverlap>* overlaps) {
  const Vector<String> kNoTags = {String()};
  std::unique_lock<Mutex> lock(blob_refs_mutex_);

  // The tags of every blob, and the sizes of the blobs.
  HashMap<String, std::set<String>> blob_tags;
  HashMap<String, ui64> blob_sizes;
  std::map<String, TagStats> tag_stats;
  std::map<Pair<String, String>, ui64> overlap_sizes;

  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    if (direct_entries_.contains(hash)) {
      continue;
    }
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
    const bool linked = blobs_.IsShared(blob);
    for (const auto& tag : tags) {
      tag_stats[tag].size += size;
      if (linked) {
        tag_stats[tag].linked_size += size;
      }
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

  lock.unlock();

  // The tags of the removed entries may have no entries anymore.
  for (const auto& loss : ghosts_->GetLosses()) {
    for (const auto& tag : loss.tags.empty() ? kNoTags : loss.tags) {
      auto& stat = tag_stats[tag];
      stat.tag = tag;
      ++stat.lost_entries;
      stat.lost_time += loss.compilation_time;
    }
  }

  for (auto& [tag, stat] : tag_stats) {
    stat.freeable_size = DoForgetTag(tag, true).freed_size;
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
  if (tag.empty()) {
    // The entries without tags - except the manifests of the direct cache.
    const auto entries = entries_->GetInEvictionOrder();
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (const auto& [entry, value] : entries) {
      auto it = entry_tags_.find(entry);
      if (!direct_entries_.contains(entry) &&
          (it == entry_tags_.end() || it->second.empty())) {
        removed.push_back(entry);
      }
    }
  } else {
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
    if (HasSharedBlobs(hash)) {
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
      SQLite::Value value;
      const ui64 size =
          (entries_->Get(entry, &value) ? std::get<SQLite::SIZE>(value) : 0) +
          GetOwnBlobsSize(hash);
      AddGhost(hash, size, GhostIndex::FORGOTTEN);
      RemoveEntry(hash);
    }
  }

  if (tag.empty()) {
    return result;
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
  ui64 size = 0;

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
  if (entry.compilation_time) {
    v1->set_compilation_time(entry.compilation_time);
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

bool FileCache::RemoveEntry(string::Hash hash) {
  String error;
  SQLite::Value entry;
  bool has_entry = entries_->Get(hash.str, &entry);
  const String common_path = CommonPath(hash);
  const String manifest_path = common_path + ".manifest";
  const String deps_path = common_path + ".d";
  const String stderr_path = common_path + ".stderr";
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
    direct_entries_.erase(hash.str.string_copy());
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
    --entry_count_;
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
      old_manifest.has_direct() &&
      old_manifest.version() == kManifestVersion) {
    variants = DirectVariants(old_manifest.direct());
  }

  proto::Manifest manifest;
  manifest.set_version(kManifestVersion);
  Variant new_variant;

  auto hash_headers = [&](const List<String>& headers,
                          bool check_time_macros) {
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
      // The hashes are memoized: the same headers are used by many sources.
      String hash;
      bool time_macros = false;
      i64 modified = 0;
      if (!file_hashes_.Hash(header_path, &hash, &error, &time_macros,
                             &modified)) {
        LOG(CACHE_INFO) << "Not storing direct cache entry - failed to hash "
                        << header_path << ": " << error;
        return false;
      }
      // The compilation may have read the previous version.
      if (start_time && modified > start_time) {
        LOG(CACHE_INFO) << "Not storing direct cache entry - " << header_path
                        << " is modified during the compilation";
        return false;
      }
      if (check_time_macros && time_macros && !is_precompiled) {
        LOG(CACHE_INFO) << "Not storing direct cache entry - " << header_path
                        << " uses the time macros";
        return false;
      }
      header_hash = Immutable(std::move(hash));
      String manifest_path = ToManifestPath(header, current_dir, root);
      hash_rope.push_back(Immutable(manifest_path));
      hash_rope.push_back(header_hash);
      new_variant.headers.push_back(std::move(manifest_path));
    }
    return true;
  };

  // The files, that could be found instead of the headers, but weren't, are
  // remembered - the others must not appear later.
  ShadowChecker checker;
  const auto prefixes = SearchPrefixes(search_dirs);
  auto collect_ignored_paths = [&](const List<String>& headers) {
    bool created = false;
    String path;
    for (const auto& header : headers) {
      const String header_path =
          (Path(header).is_absolute() ? Path(header) : current_dir / header)
              .lexically_normal()
              .native();
      VisitShadowingPaths(
          header_path, prefixes,
          [&](const String& prefix, std::string_view relative) {
            struct stat st;
            if (created || !checker.IsFile(prefix, relative, &path, &st)) {
              return;
            }
            // The compiler may have missed the file, that is created during
            // the compilation.
            if (start_time && base::StatusChangeTime(st) > start_time) {
              LOG(CACHE_INFO) << "Not storing direct cache entry - " << path
                              << " is created during the compilation";
              created = true;
              return;
            }
            new_variant.ignored_paths.insert(
                ToManifestPath(path, current_dir, root));
          });
      if (created) {
        return false;
      }
    }
    return true;
  };

  // The result depends on the time, if these macros are used - the direct
  // cache can't detect it. The simple cache is still fine, since the macros
  // are expanded in the preprocessed source.
  if (!hash_headers(headers, true) ||
      !hash_headers(preprocessed_headers, false) ||
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
  ghosts_->BeginTransaction();
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
      // The entry may be removed already.
      EntryInfo info;
      if (!LoadEntryInfo(hash, &info)) {
        continue;
      }
      AddBlobRefs(hash, info, access->time);
      // Insert a new entry, or replace the stored again one.
      const bool exists = entries_->Get(hash.str, &entry);
      if (exists) {
        cache_size_ -= std::get<SQLite::SIZE>(entry);
      } else {
        ++entry_count_;
      }
      if (!entries_->Set(
              hash.str,
              std::make_tuple(access->time, info.size, kManifestVersion,
                              exists ? std::get<SQLite::HITS>(entry) : 0u,
                              ui32(info.direct), JoinBlobs(info.blobs)))) {
        LOG(CACHE_ERROR) << "Failed to update the index for " << hash.str;
      }
      cache_size_ += info.size;
      STAT(CACHE_SIZE_ADDED, info.size);
      // The removed entry would have been a hit.
      if (!exists && ghosts_ttl_ && !info.object.empty() &&
          ghosts_->Resurrect(info.object, info.compilation_time,
                             access->time)) {
        LOG(CACHE_VERBOSE) << "The removed entry is needed again: "
                           << hash.str;
      }
    }
  }

  ghosts_->EndTransaction();
  tag_index_->EndTransaction();
  entries_->EndTransaction();

  if (Clock::now() >= next_sizes_report_) {
    ReportSizes();
  }
}

void FileCache::Maintain() {
  entries_->BeginTransaction();
  tag_index_->BeginTransaction();
  ghosts_->BeginTransaction();
  Evict();
  CompressColdEntries();
  const ui64 now = time(nullptr);
  ghosts_->RemoveOlderThan(ghosts_ttl_ ? now - std::min(now, ghosts_ttl_)
                                       : now + 1);
  ghosts_->EndTransaction();
  tag_index_->EndTransaction();
  entries_->EndTransaction();
  ReportSizes();
}

void FileCache::AddGhost(string::Hash hash, ui64 size,
                         GhostIndex::Reason reason) {
  if (!ghosts_ttl_) {
    return;
  }
  // Only the entries with the objects: they are found by them.
  EntryInfo info;
  if (!LoadEntryInfo(hash, &info) || info.object.empty()) {
    return;
  }
  GhostIndex::Ghost ghost;
  ghost.object = info.object;
  ghost.size = size;
  ghost.compilation_time = info.compilation_time;
  ghost.reason = reason;
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    auto it = entry_tags_.find(hash.str.string_copy());
    if (it != entry_tags_.end()) {
      ghost.tags = it->second;
    }
  }
  ghosts_->Add(ghost, time(nullptr));
}

bool FileCache::ForgetGhosts(Fn<void(ui64)> callback, NeededFn needed) {
  return bool(cleaner_.Push([this, callback, needed] {
    if (!needed()) {
      return;
    }
    const ui64 count = ghosts_->Clear();
    ReportSizes();
    callback(count);
  }));
}

void FileCache::ReportSizes() {
  const auto start = Clock::now();
  BlobStore::Sizes sizes;
  {
    // Every blob counts once.
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    blobs_.GetSizes(&sizes);
  }

  using M = perf::proto::Metric;
  auto& stats = base::Singleton<perf::StatService>::Get();
  stats.Set(M::CACHE_OWN_SIZE, cache_size_ + sizes.own);
  stats.Set(M::CACHE_SHARED_SIZE, sizes.shared);
  stats.Set(M::CACHE_ENTRIES, entry_count_);
  stats.Set(M::CACHE_MAX_SIZE, max_size_);
  stats.Set(M::DISK_SAVED_BY_DEDUPLICATION, sizes.deduplicated);
  stats.Set(M::DISK_SAVED_BY_LINKS, sizes.linked);

  // The removed entries, that are needed again.
  ui64 evicted = 0, evicted_time = 0, forgotten = 0, forgotten_time = 0;
  Vector<ui64> extra_sizes;
  for (const auto& loss : ghosts_->GetLosses()) {
    if (loss.reason == GhostIndex::EVICTED) {
      ++evicted;
      evicted_time += loss.compilation_time;
      extra_sizes.push_back(loss.extra_size);
    } else {
      ++forgotten;
      forgotten_time += loss.compilation_time;
    }
  }
  // How much larger cache would have kept the part of the evicted entries.
  std::sort(extra_sizes.begin(), extra_sizes.end());
  auto ExtraSize = [&extra_sizes](ui32 percent) -> ui64 {
    if (extra_sizes.empty()) {
      return 0;
    }
    const size_t count = (extra_sizes.size() * percent + 99) / 100;
    return extra_sizes[std::max<size_t>(count, 1) - 1];
  };
  stats.Set(M::GHOSTS, ghosts_->Count());
  stats.Set(M::EVICTED_NEEDED_AGAIN, evicted);
  stats.Set(M::TIME_LOST_TO_EVICTION, evicted_time);
  stats.Set(M::EXTRA_SIZE_FOR_HALF_LOSSES, ExtraSize(50));
  stats.Set(M::EXTRA_SIZE_FOR_90_PERCENT_LOSSES, ExtraSize(90));
  stats.Set(M::EXTRA_SIZE_FOR_ALL_LOSSES, ExtraSize(100));
  stats.Set(M::FORGOTTEN_NEEDED_AGAIN, forgotten);
  stats.Set(M::TIME_LOST_TO_FORGETTING, forgotten_time);
  const auto now = Clock::now();
  next_sizes_report_ = now + (now - start) * kSizesReportCost;
}

ui32 FileCache::CompressColdEntries() {
  if (!compress_after_) {
    return 0;
  }

  // Don't hold the cleaner for too long: the requests of the tags wait for
  // it, and the rest is compressed on the next passes.
  const ui64 kMaxCompressedSizePerPass = 256ull << 20;
  const auto kMaxTimePerPass = std::chrono::seconds(2);
  const ui64 now = time(nullptr);
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  return blobs_.CompressUnused(now - std::min(now, compress_after_),
                               kMaxCompressedSizePerPass,
                               Clock::now() + kMaxTimePerPass);
}

bool FileCache::LoadEntryInfo(string::Hash hash, EntryInfo* info) const {
  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  proto::Manifest manifest;
  if (!base::LoadFromFile(manifest_path, &manifest) ||
      manifest.version() != kManifestVersion ||
      (!manifest.has_v1() && !manifest.has_direct())) {
    return false;
  }

  info->direct = manifest.has_direct();
  info->size = manifest.v1().size() + base::File::Size(manifest_path);
  if (manifest.v1().obj()) {
    info->blobs.push_back(manifest.v1().object_hash());
    info->object = manifest.v1().object_hash();
  }
  info->compilation_time = manifest.v1().compilation_time();
  if (manifest.v1().dwo()) {
    info->blobs.push_back(manifest.v1().dwo_hash());
  }
  return true;
}

void FileCache::AddBlobRefs(string::Hash hash, const EntryInfo& info,
                            ui64 access_time) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  if (info.direct) {
    direct_entries_.insert(hash.str.string_copy());
  }
  // The new references go first: the blobs may be the same.
  for (const auto& blob : info.blobs) {
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
  if (!info.blobs.empty()) {
    entry_blobs_.emplace(key, info.blobs);
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
  // The index keeps the blobs of the entries - the manifests aren't read.
  for (const auto& [hash, entry] : entries_->GetInEvictionOrder()) {
    EntryInfo info;
    info.direct = std::get<SQLite::DIRECT>(entry);
    info.blobs = SplitBlobs(std::get<SQLite::BLOBS>(entry));
    AddBlobRefs(string::Hash(Immutable(hash)), info,
                std::get<SQLite::MTIME>(entry));
    cache_size_ += std::get<SQLite::SIZE>(entry);
    ++entry_count_;
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
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    blobs_.RemoveUnreferenced(start_time);
  }

  ReportSizes();
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

void FileCache::Evict() {
  if (max_size_ == UNLIMITED && !min_free_percent_) {
    return;
  }

  // The entries, which outputs are shared with build directories, are free:
  // their removal doesn't free the disk space. They are likely to be used
  // soon, so they are never removed.
  auto entries = entries_->GetInEvictionOrder();
  ui64 own_size = 0;
  Vector<ui64> own_sizes;
  own_sizes.reserve(entries.size());
  for (const auto& [hash_str, entry] : entries) {
    const string::Hash hash{Immutable(hash_str)};
    const ui64 size = std::get<SQLite::SIZE>(entry);
    own_sizes.push_back(HasSharedBlobs(hash) ? 0 : size);
    own_size += size;
  }
  {
    // Every blob counts once.
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    BlobStore::Sizes sizes;
    blobs_.GetSizes(&sizes);
    own_size += sizes.own;
  }

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
      AddGhost(hash, removed_size, GhostIndex::EVICTED);
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
