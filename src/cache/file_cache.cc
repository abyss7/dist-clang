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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

// The keys of the direct cache differ from the ones of the simple cache with
// the same text. Any change of the keys raises the generation of the cache.
const Literal kDirectKeyMarker = "direct"_l;

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

// The path of the header in the manifest: relative to the build directory, if
// the header is under it - e.g. a generated one: another build directory has
// its own; otherwise relative to the source root, if the header is under it.
String ToManifestPath(const String& header, const Path& current_dir,
                      const String& root) {
  if (root.empty()) {
    return header;
  }
  const String& dir = current_dir.native();
  const String path = base::NormalPath(dir, header);
  const String build_dir = base::NormalPath(dir, String());
  if (path.starts_with(build_dir + "/")) {
    return path.substr(build_dir.size() + 1);
  }
  if (path.starts_with(root + "/")) {
    return kRootPlaceholder + path.substr(root.size());
  }
  return header;
}

// The normal path of the header to read.
String FromManifestPath(const String& header, const Path& current_dir,
                        const String& root) {
  if (header.starts_with(kRootPlaceholder)) {
    // Can't be resolved without the root - the empty path is never read.
    return root.empty() ? String()
                        : root + header.substr(strlen(kRootPlaceholder));
  }
  return base::NormalPath(current_dir.native(), header);
}

struct Variant {
  List<String> headers;
  HashSet<String> ignored_paths;
  std::set<String> scopes;  // Sorted - for the stable manifest.
};

List<Variant> DirectVariants(const proto::Direct& direct) {
  List<Variant> variants;
  for (const auto& variant : direct.variants()) {
    variants.push_back(
        {{variant.headers().begin(), variant.headers().end()},
         {variant.ignored_paths().begin(), variant.ignored_paths().end()},
         {variant.scopes().begin(), variant.scopes().end()}});
  }
  return variants;
}

// The row of the direct cache, that only the lookups in the |scope| see.
String ScopedRow(const String& scope, const String& direct_hash) {
  return "scope:" + scope + ":" + direct_hash;
}

// The prefix of the trace of the time among the blobs of the entry in the
// index: the size of the traces is shown apart.
const char kTimeTracePrefix[] = "trace:";

// The prefix of the entries, that the direct entry leads to - in the same
// column of the index.
const char kTargetPrefix[] = "entry:";

// The blobs of the entry in the index - one of them may be the |time_trace| -
// and the |targets| of the direct entry.
String JoinBlobs(const Vector<String>& blobs, const String& time_trace,
                 const Vector<String>& targets = {}) {
  String result;
  for (const auto& blob : blobs) {
    result += (result.empty() ? "" : "\n") +
              (blob == time_trace ? kTimeTracePrefix + blob : blob);
  }
  for (const auto& target : targets) {
    result += (result.empty() ? "" : "\n") + (kTargetPrefix + target);
  }
  return result;
}

Vector<String> SplitBlobs(const String& blobs, String* time_trace,
                          Vector<String>* targets = nullptr) {
  List<String> list;
  base::SplitString<'\n'>(blobs, list);
  Vector<String> result;
  for (auto& blob : list) {
    if (blob.starts_with(kTargetPrefix)) {
      if (targets) {
        targets->push_back(blob.substr(strlen(kTargetPrefix)));
      }
      continue;
    }
    if (blob.starts_with(kTimeTracePrefix)) {
      blob.erase(0, strlen(kTimeTracePrefix));
      *time_trace = blob;
    }
    result.push_back(blob);
  }
  return result;
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

// Checks the files, that could shadow the headers. Most of them don't exist:
// the names are looked up in the listings of the directories on the way to a
// file - each directory is checked once - and only the found ones are checked
// on the disk.
class ShadowChecker {
 public:
  explicit ShadowChecker(DirectoryListings* listings) : listings_(listings) {}

  // Returns the path of the file |prefix| + |relative|, if it exists.
  bool IsFile(const String& prefix, std::string_view relative, String* path,
              struct stat* st) {
    path->assign(prefix);
    size_t start = 0;
    while (true) {
      const size_t end = relative.find('/', start);
      const std::string_view name =
          relative.substr(start, end == std::string_view::npos
                                     ? std::string_view::npos
                                     : end - start);
      const auto* listing = Listing(*path);
      if (!listing || !listing->contains(base::PathKey(String(name)))) {
        return false;
      }
      path->append(name);
      if (end == std::string_view::npos) {
        break;
      }
      path->push_back('/');
      start = end + 1;
    }
    return cache::IsFile(*path, st);
  }

 private:
  // The listing of the directory |path| with the trailing separator.
  const DirectoryListings::Listing* Listing(const String& path) {
    auto it = listings_of_lookup_.find(path);
    if (it == listings_of_lookup_.end()) {
      String dir = path;
      if (dir.size() > 1) {
        dir.pop_back();
      }
      it = listings_of_lookup_.emplace(path, listings_->Get(dir)).first;
    }
    return it->second.get();
  }

  DirectoryListings* const listings_;
  HashMap<String, SharedPtr<const DirectoryListings::Listing>>
      listings_of_lookup_;
};

// A quoted include is searched in the directory of the including file first
// - before all search directories. The including files aren't known: the
// directories of all headers are checked, but only the ones, that have the
// name of a header.
class IncluderDirs {
 public:
  IncluderDirs(const Vector<String>& header_paths,
               const Vector<String>& prefixes, DirectoryListings* listings)
      : search_dirs_(prefixes.begin(), prefixes.end()),
        headers_(header_paths.begin(), header_paths.end()) {
    HashSet<String> first_names;
    for (const auto& header : header_paths) {
      VisitRelative(header, [&](std::string_view relative) {
        first_names.insert(FirstName(relative));
      });
    }
    if (first_names.empty()) {
      return;
    }
    HashSet<String> seen;
    for (const auto& header : header_paths) {
      const size_t slash = header.rfind('/');
      if (slash == String::npos || slash == 0) {
        continue;
      }
      String dir = header.substr(0, slash);
      if (!seen.insert(dir).second) {
        continue;
      }
      const auto listing = listings->Get(dir);
      if (!listing) {
        continue;
      }
      dir.push_back('/');
      auto Add = [&](const String& name) {
        dirs_by_first_name_[name].push_back(dir);
      };
      if (listing->size() < first_names.size()) {
        for (const auto& name : *listing) {
          if (first_names.contains(name)) {
            Add(name);
          }
        }
      } else {
        for (const auto& name : first_names) {
          if (listing->contains(name)) {
            Add(name);
          }
        }
      }
    }
  }

  // Calls |visit(prefix, relative)| for the paths, where a new file would be
  // found instead of |header| by a quoted include from another header.
  template <class Visit>
  void VisitShadowingPaths(const String& header, Visit visit) const {
    if (dirs_by_first_name_.empty()) {
      return;
    }
    VisitRelative(header, [&](std::string_view relative) {
      auto it = dirs_by_first_name_.find(FirstName(relative));
      if (it == dirs_by_first_name_.end()) {
        return;
      }
      for (const auto& dir : it->second) {
        // The headers themselves are hashed: they're not new files.
        const String path = dir + String(relative);
        if (!headers_.contains(path)) {
          visit(dir, relative);
        }
      }
    });
  }

 private:
  // The header is found by its name in a search directory - e.g. "util.h":
  // the paths with the directories are rarely included relatively to the
  // including file, and the checks of them would take much more time.
  template <class Visit>
  void VisitRelative(const String& header, Visit visit) const {
    const size_t slash = header.rfind('/');
    if (slash == String::npos) {
      return;
    }
    const std::string_view dir(header.c_str(), slash + 1);
    const std::string_view name(header.c_str() + slash + 1,
                                header.size() - slash - 1);
    if (search_dirs_.contains(dir) && !IsSource(name)) {
      visit(name);
    }
  }

  // The dependencies have the compiled source too - it isn't included.
  static bool IsSource(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
      return false;
    }
    const auto extension = name.substr(dot + 1);
    for (const char* source : {"c", "cc", "cpp", "cxx", "c++", "m", "mm", "cu"}) {
      if (extension == source) {
        return true;
      }
    }
    return false;
  }

  static String FirstName(std::string_view name) {
    return base::PathKey(String(name));
  }

  const HashSet<std::string_view> search_dirs_;
  // Of |header_paths|, that outlive this object.
  const HashSet<std::string_view> headers_;
  HashMap<String, Vector<String>> dirs_by_first_name_;
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
  SharedPtr<EntryList> old_entries;
  std::lock_guard<Mutex> lock(new_entries_mutex_);
  old_entries = std::move(new_entries_);
}

namespace {

const char kFormatFile[] = "format";
// Exists, while the daemon uses the cache: after a crash it's left.
const char kRunningFile[] = "running";
const char kLockFile[] = "lock";
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
  return name == "blobs" || name == kFormatFile || name == kRunningFile ||
         name == kLockFile;
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

FileCache::DirectoryLock::~DirectoryLock() {
  if (fd != -1) {
    close(fd);
  }
}

FileCache::RunningMarker::~RunningMarker() {
  if (!path.empty()) {
    base::File::Delete(path);
  }
}

bool FileCache::CheckFormat() {
  const Path format_path = path_ / kFormatFile;
  proto::Format format;
  bool save = false;

  if (base::File::Exists(format_path)) {
    // The newer revisions may add the fields.
    if (!base::LoadFromFile(format_path, &format, nullptr, true)) {
      // Can't tell anything - e.g. it's written by a newer daemon, or by
      // hand: the cache isn't removed, but isn't used either.
      LOG(CACHE_ERROR) << "Can't read the format of the cache " << format_path
                       << ": the cache isn't used - fix or remove the file";
      base::Singleton<perf::StatService>::Get().Set(
          perf::proto::Metric::CACHE_INCOMPATIBLE, 1);
      return false;
    }
  } else if (base::File::Exists(path_ / "index.sqlite") ||
             base::File::Exists(path_ / "direct.sqlite")) {
    // The cache before the generations.
    format.set_generation(1);
    format.set_revision(0);
  } else {
    format.set_generation(kGeneration);
    format.set_revision(kRevision);
    save = true;
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
      // The lock of this daemon stays.
      if (!IsCacheFile(name) || name == kLockFile) {
        continue;
      }
      if (!moved || !base::File::Move(entry.path(), removed_path / name)) {
        std::filesystem::remove_all(entry.path(), ec);
      }
    }
    format.Clear();
    format.set_generation(kGeneration);
    format.set_revision(kRevision);
    save = true;
  }

  // The newer revisions of the same generation stay: their daemons may still
  // be used.
  if (format.revision() < kRevision) {
    format.set_revision(kRevision);
    save = true;
  }
  if (save) {
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
         base::File::Size(path_ / kRunningFile) +
         base::File::Size(path_ / kLockFile);
}

namespace {

// Checks the name of the files of the entries: "<32 hex>-<16 hex>-<16 hex>".
bool IsEntryHash(const String& name) {
  if (name.size() != 66 || name[32] != '-' || name[49] != '-') {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    if (i != 32 && i != 49 && !isxdigit(name[i])) {
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
  {
    std::unique_lock<std::shared_mutex> stores_lock(stores_mutex_);
    closed_ = true;
  }
  resetter_.reset();
  FlushAccesses();
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
                                       info.direct, JoinBlobs(info.blobs, info.time_trace, info.targets)))) {
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

  // Only one daemon works with the cache - before any changes of its files.
  const Path lock_path = path_ / kLockFile;
  const int lock_fd =
      open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd == -1) {
    base::GetLastError(&error);
    LOG(CACHE_ERROR) << "Failed to open " << lock_path << ": " << error;
    return false;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
    Immutable owner;
    base::File::Read(lock_path, &owner);
    LOG(CACHE_ERROR) << "The cache " << path_
                     << " is used by another daemon, pid " << owner
                     << ": it isn't used";
    close(lock_fd);
    return false;
  }
  directory_lock_.fd = lock_fd;
  const String pid = std::to_string(getpid());
  if (ftruncate(lock_fd, 0) != 0 ||
      pwrite(lock_fd, pid.data(), pid.size(), 0) != ssize_t(pid.size())) {
    LOG(CACHE_WARNING) << "Failed to write the pid into " << lock_path;
  }

  if (!CheckFormat()) {
    return false;
  }
  RemoveOldCaches(path_);

  // The databases are read whole only after a crash: otherwise it takes
  // seconds at every start of the big cache.
  const Path running_path = path_ / kRunningFile;
  const bool crashed = base::File::Exists(running_path);
  database_.reset(new SQLiteKV(path_, "direct", crashed));
  tag_index_.reset(new TagIndex(path_, "tags", crashed));
  ghosts_.reset(new GhostIndex(path_, "ghosts", crashed));
  if (store_index_) {
    entries_.reset(new SQLite(path_, "index", crashed));
  } else {
    entries_.reset(new SQLite);
  }

  // The files are checked only after a crash, or with a new index: otherwise
  // the index has all entries.
  const bool check_files = entries_->created() || crashed;
  if (!base::File::Write(running_path, Immutable(std::to_string(getpid())),
                         &error)) {
    LOG(CACHE_ERROR) << "Failed to write " << running_path << ": " << error;
    return false;
  }
  running_marker_.path = running_path;

  CHECK(clean_period > 0);

  FlushAccesses();

  const ui64 flush_period = std::min(clean_period, flush_period_);
  base::WorkerPool::SimpleWorker worker =
      [this, clean_period, flush_period](const base::WorkerPool& pool) {
        ui64 elapsed = 0;
        while (!pool.WaitUntilShutdown(Seconds(flush_period))) {
          FlushAccesses();
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
    if (!store_time_traces_) {
      RemoveTimeTraces();
    }
    entries_->BeginTransaction();
    tag_index_->BeginTransaction();
    RemoveUnreachableDirectEntries();
    tag_index_->EndTransaction();
    entries_->EndTransaction();
  });
  cleaner_.Run();

  return true;
}

using namespace string;

// static
HandledHash FileCache::Hash(HandledSource code, const ExtraFiles& extra_files,
                            CommandLine command_line, Version version) {
  return HandledHash(HashCombine(code.str, extra_files) + "-" +
                     base::Hexify(command_line.str.Hash(8)) + "-" +
                     base::Hexify(version.str.Hash(8)));
}

// static
UnhandledHash FileCache::Hash(UnhandledSource code,
                              const ExtraFiles& extra_files,
                              CommandLine command_line, Version version) {
  return UnhandledHash(
      HashCombine(code.str, extra_files) + "-" +
      base::Hexify(command_line.str.Hash(8)) + "-" +
      base::Hexify((version.str + "\n"_l + kDirectKeyMarker).Hash(8)));
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

  ShadowChecker checker(&directory_listings_);
  const auto prefixes = SearchPrefixes(search_dirs);

  // The first directory, that has a name: only the paths, that start with
  // such a name before the directory of the header, are checked.
  HashMap<std::string_view, size_t> first_dirs;
  Vector<SharedPtr<const DirectoryListings::Listing>> listings;
  for (size_t dir = 0; dir < prefixes.size(); ++dir) {
    String path = prefixes[dir];
    if (path.size() > 1) {
      path.pop_back();
    }
    listings.push_back(directory_listings_.Get(path));
    if (listings.back()) {
      for (const auto& name : *listings.back()) {
        first_dirs.emplace(name, dir);
      }
    }
  }

  Vector<String> header_paths;
  header_paths.reserve(headers.size());
  for (const auto& header : headers) {
    header_paths.push_back(FromManifestPath(header, current_dir, root));
  }
  const IncluderDirs includer_dirs(header_paths, prefixes,
                                   &directory_listings_);

  String path;
  for (const auto& header_path : header_paths) {
    bool shadowed = false;
    includer_dirs.VisitShadowingPaths(
        header_path, [&](const String& prefix, std::string_view relative) {
          struct stat st;
          if (!shadowed && checker.IsFile(prefix, relative, &path, &st) &&
              !ignored_paths.contains(
                  ToManifestPath(path, current_dir, root))) {
            LOG(CACHE_INFO) << "Header " << header_path
                            << " is shadowed by " << path;
            shadowed = true;
          }
        });
    if (shadowed) {
      return true;
    }
    for (size_t dir = 0; dir < prefixes.size(); ++dir) {
      const String& prefix = prefixes[dir];
      if (header_path.size() <= prefix.size() ||
          header_path.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      const std::string_view relative(header_path.c_str() + prefix.size(),
                                      header_path.size() - prefix.size());
      const String first_name =
          base::PathKey(String(relative.substr(0, relative.find('/'))));
      auto it = first_dirs.find(first_name);
      if (it == first_dirs.end()) {
        continue;
      }
      // The header is found in this directory - then a file with the same
      // relative path in any previous directory is found instead.
      for (size_t other_dir = it->second; other_dir < dir; ++other_dir) {
        struct stat st;
        if (checker.IsFile(prefixes[other_dir], relative, &path, &st) &&
            !ignored_paths.contains(ToManifestPath(path, current_dir, root))) {
          LOG(CACHE_INFO) << "Header " << header_path << " is shadowed by "
                          << path;
          return true;
        }
      }
    }
  }

  return false;
}

bool FileCache::Find(UnhandledSource code, const ExtraFiles& extra_files,
                     CommandLine command_line, Version version,
                     const Path& current_dir, Entry* entry,
                     const String& root, const List<Path>& search_dirs,
                     const String& scope) const {
  DCHECK(entry);

  auto unhandled_hash = Hash(code, extra_files, command_line, version);
  const auto manifest_path =
      AppendExtension(CommonPath(unhandled_hash), base::kExtManifest);
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!base::LoadBinaryFromFile(manifest_path, &manifest) || !manifest.has_direct() ||
      manifest.version() != kManifestVersion) {
    return false;
  }

  // Try all known sets of headers: the same source may include different
  // headers, e.g. on different branches.
  for (const auto& variant : DirectVariants(manifest.direct())) {
    if (!scope.empty() && !variant.scopes.contains(scope)) {
      continue;
    }
    Immutable::Rope hash_rope = {unhandled_hash};
    bool hashed = true;
    for (const auto& header : variant.headers) {
      String header_hash;
      const String header_path = FromManifestPath(header, current_dir, root);
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

    const String hash_with_headers =
        base::Hexify(Immutable(hash_rope).Hash());
    Immutable handled_hash;

    DCHECK(database_);
    if (database_->Get(scope.empty() ? hash_with_headers
                                     : ScopedRow(scope, hash_with_headers),
                       &handled_hash) &&
        !IsShadowed(variant.headers, variant.ignored_paths, current_dir, root,
                    search_dirs) &&
        Find(HandledHash(handled_hash), entry, scope)) {
      utime(manifest_path.c_str(), nullptr);
      NewEntries()->Append({ui64(time(nullptr)), unhandled_hash, true});
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

bool FileCache::Find(HandledHash hash, Entry* found,
                     const String& scope) const {
  DCHECK(found);

  if (!scope.empty() && !InScope(hash.str.string_copy(), scope)) {
    return false;
  }

  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  const ReadLock lock(this, manifest_path);

  if (!lock) {
    return false;
  }

  proto::Manifest manifest;
  if (!base::LoadBinaryFromFile(manifest_path, &manifest) || !manifest.has_v1() ||
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

  // The blobs of the hit may be not counted yet - before they're read.
  if (v1.obj()) {
    blobs_.MarkUsed(v1.object_hash());
  }
  if (v1.dwo()) {
    blobs_.MarkUsed(v1.dwo_hash());
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
  // The lost trace doesn't break the entry: it's read only, if it's needed.
  if (v1.has_time_trace_hash()) {
    blobs_.MarkUsed(v1.time_trace_hash());
    entry->time_trace_hash = v1.time_trace_hash();
    entry->time_trace_options = v1.time_trace_options();
  }
  entry->warnings_hash = v1.warnings_hash();
  entry->compilation_time = v1.compilation_time();
  entry->handled_hash = hash.str.string_copy();

  if (!v1.has_size() || v1.size() != size) {
    return false;
  }

  utime(manifest_path.c_str(), nullptr);
  NewEntries()->Append({ui64(time(nullptr)), hash, true});
  // The strings of the entry are assigned only once - replace it whole.
  std::destroy_at(found);
  std::construct_at(found, std::move(result));
  return true;
}

bool FileCache::IsVerified(const String& blob_hash) const {
  return blobs_.IsVerified(blob_hash);
}

void FileCache::Restored(const String& blob_hash) const {
  blobs_.Restored(blob_hash);
}

bool FileCache::FindTimeTrace(const String& hash, Immutable* trace) const {
  ui64 size = 0;
  return !hash.empty() && blobs_.Get(hash, trace, nullptr, &size);
}

void FileCache::Store(UnhandledSource code, const ExtraFiles& extra_files,
                      CommandLine command_line, Version version,
                      const List<String>& headers,
                      const List<String>& preprocessed_headers,
                      const Path& current_dir, string::HandledHash hash,
                      const String& root, const List<Path>& search_dirs,
                      i64 start_time, const String& scope) {
  std::shared_lock<std::shared_mutex> stores_lock(stores_mutex_);
  if (closed_) {
    return;
  }

  // The macros of the time may come with the definitions of other macros.
  for (const auto macro : {"__DATE__"_l, "__TIME__"_l, "__TIMESTAMP__"_l}) {
    if (command_line.str.find(macro) != String::npos) {
      LOG(CACHE_INFO) << "Not storing direct cache entry - the flags use "
                      << macro;
      return;
    }
  }
  DoStore(Hash(code, extra_files, command_line, version), headers,
          preprocessed_headers, current_dir, hash, root, search_dirs,
          start_time, scope);
}

void FileCache::FlushAccesses() {
  SharedPtr<EntryList> old_entries;
  {
    std::lock_guard<Mutex> lock(new_entries_mutex_);
    old_entries = std::move(new_entries_);
    new_entries_.reset(new EntryList, new_entries_deleter_);
  }
  // The last owner gives the old list to the cleaner - outside of the lock.
}

SharedPtr<FileCache::EntryList> FileCache::NewEntries() const {
  std::lock_guard<Mutex> lock(new_entries_mutex_);
  return new_entries_;
}

void FileCache::AddTags(string::HandledHash hash, const Vector<String>& tags) {
  if (tags.empty()) {
    return;
  }
  NewEntries()->Append({ui64(time(nullptr)), hash, true, tags});

  for (const auto& tag : tags) {
    if (isolated_tags_.contains(tag)) {
      std::unique_lock<std::shared_mutex> lock(scopes_mutex_);
      scopes_[tag].insert(hash.str.string_copy());
    }
  }
}

bool FileCache::IsIsolated(const String& entry) const {
  std::shared_lock<std::shared_mutex> lock(scopes_mutex_);
  for (const auto& [tag, entries] : scopes_) {
    if (entries.contains(entry)) {
      return true;
    }
  }
  return false;
}

bool FileCache::IsHeld(const String& entry) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  auto it = entry_tags_.find(entry);
  return it != entry_tags_.end() &&
         std::any_of(it->second.begin(), it->second.end(),
                     [this](const String& tag) {
                       return tag_quotas_.contains(tag);
                     });
}

HashSet<String> FileCache::HeldEntries() {
  HashSet<String> held;
  if (tag_quotas_.empty()) {
    return held;
  }
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  for (const auto& [entry, tags] : entry_tags_) {
    for (const auto& tag : tags) {
      if (tag_quotas_.contains(tag)) {
        held.insert(entry);
        break;
      }
    }
  }
  Vector<String> direct;
  for (const auto& [entry, targets] : direct_entries_) {
    if (std::any_of(targets.begin(), targets.end(),
                    [&held](const String& target) {
                      return held.contains(target);
                    })) {
      direct.push_back(entry);
    }
  }
  held.insert(direct.begin(), direct.end());
  return held;
}

bool FileCache::InScope(const String& entry, const String& scope) const {
  std::shared_lock<std::shared_mutex> lock(scopes_mutex_);
  auto it = scopes_.find(scope);
  return it != scopes_.end() && it->second.contains(entry);
}

void FileCache::UpdateScope(const String& tag, bool keep_others) {
  if (!isolated_tags_.contains(tag)) {
    return;
  }
  HashSet<String> scope;
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (const auto& [entry, tags] : entry_tags_) {
      if (std::binary_search(tags.begin(), tags.end(), tag)) {
        scope.insert(entry);
      }
    }
  }
  std::unique_lock<std::shared_mutex> lock(scopes_mutex_);
  if (keep_others) {
    scopes_[tag].merge(scope);
  } else {
    scopes_[tag] = std::move(scope);
  }
}

void FileCache::ApplyTags(string::Hash hash, const Vector<String>& tags,
                          ui64 time) {
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
  for (const auto& tag : tags) {
    auto& tag_time = tag_times_[tag][key];
    if (time > tag_time) {
      tag_time = time;
      tag_index_->SetTime(key, tag, time);
    }
  }
  // Again - e.g. after the scopes are updated, while the tag was on the way.
  for (const auto& tag : tags) {
    if (isolated_tags_.contains(tag)) {
      std::unique_lock<std::shared_mutex> lock(scopes_mutex_);
      scopes_[tag].insert(key);
    }
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
  FlushAccesses();
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
  FlushAccesses();
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

  // The direct entries count for the tags of the entries, that they lead to.
  HashSet<String> indexed;
  const auto entries = entries_->GetInEvictionOrder();
  for (const auto& [hash, value] : entries) {
    if (!direct_entries_.contains(hash)) {
      indexed.insert(hash);
    }
  }
  for (const auto& [hash, value] : entries) {
    Vector<String> direct_tags;
    if (auto direct_it = direct_entries_.find(hash);
        direct_it != direct_entries_.end()) {
      std::set<String> all_tags;
      bool reachable = false;
      for (const auto& target : direct_it->second) {
        if (!indexed.contains(target)) {
          continue;
        }
        reachable = true;
        auto tags_it = entry_tags_.find(target);
        if (tags_it == entry_tags_.end() || tags_it->second.empty()) {
          all_tags.insert(String());
        } else {
          all_tags.insert(tags_it->second.begin(), tags_it->second.end());
        }
      }
      // It's removed by the maintenance.
      if (!reachable) {
        continue;
      }
      direct_tags.assign(all_tags.begin(), all_tags.end());
    }
    const bool direct = !direct_tags.empty();
    auto tags_it = entry_tags_.find(hash);
    const auto& tags =
        direct ? direct_tags
        : tags_it == entry_tags_.end() || tags_it->second.empty()
            ? kNoTags
            : tags_it->second;
    const ui64 size = std::get<SQLite::SIZE>(value);
    for (const auto& tag : tags) {
      auto& stat = tag_stats[tag];
      stat.tag = tag;
      stat.size += size;
      if (!direct) {
        ++stat.entries;
      }
      if (tags.size() == 1) {
        stat.exclusive_size += size;
        if (!direct) {
          ++stat.exclusive_entries;
        }
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

  for (const auto& [tag, quota] : tag_quotas_) {
    tag_stats[tag].tag = tag;
  }
  for (auto& [tag, stat] : tag_stats) {
    stat.isolated = isolated_tags_.contains(tag);
    if (auto it = tag_quotas_.find(tag); it != tag_quotas_.end()) {
      stat.quota = it->second;
    }
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

  // The direct entries, that lead only to the removed entries, find nothing.
  List<String> removed_direct;
  {
    const HashSet<String> removed_set(removed.begin(), removed.end());
    HashSet<String> indexed;
    const auto entries = entries_->GetInEvictionOrder();
    for (const auto& [entry, value] : entries) {
      if (!std::get<SQLite::DIRECT>(value)) {
        indexed.insert(entry);
      }
    }
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (const auto& [entry, targets] : direct_entries_) {
      bool leads_to_removed = false, leads_to_others = false;
      for (const auto& target : targets) {
        if (removed_set.contains(target)) {
          leads_to_removed = true;
        } else if (indexed.contains(target)) {
          leads_to_others = true;
        }
      }
      if (leads_to_removed && !leads_to_others) {
        removed_direct.push_back(entry);
      }
    }
    for (const auto& entry : removed_direct) {
      SQLite::Value value;
      if (entries_->Get(entry, &value)) {
        result.freed_size += std::get<SQLite::SIZE>(value);
      }
    }
  }

  if (dry_run) {
    return result;
  }

  // The direct entries aren't remembered as the removed ones: they aren't
  // compilations.
  for (const auto& entry : removed_direct) {
    const string::Hash hash{Immutable(entry)};
    WriteLock lock(this, AppendExtension(CommonPath(hash), base::kExtManifest));
    if (lock) {
      RemoveEntry(hash);
    }
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
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (auto& [entry, tags] : entry_tags_) {
      auto it = std::lower_bound(tags.begin(), tags.end(), tag);
      if (it != tags.end() && *it == tag && tags.size() > 1) {
        tags.erase(it);
        tag_index_->Set(entry, tags);
        tag_times_[tag].erase(entry);
        tag_index_->DeleteTime(entry, tag);
      }
    }
  }
  // The linked entries keep the tag.
  UpdateScope(tag, false);

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
  std::shared_lock<std::shared_mutex> stores_lock(stores_mutex_);
  if (closed_) {
    return;
  }

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

  // The blobs, that are put by this store: nothing refers to them on a
  // failure.
  List<String> new_blobs;
  auto Fail = [&] {
    RemoveEntry(hash);
    for (const auto& blob : new_blobs) {
      blobs_.Discard(blob);
    }
  };

  v1->set_err(!entry.stderr.empty());
  if (!entry.stderr.empty()) {
    const auto stderr_path =
        AppendExtension(CommonPath(hash), base::kExtStderr);

    if (!base::File::Write(stderr_path, entry.stderr, &error)) {
      Fail();
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
      Fail();
      LOG(CACHE_ERROR) << "Failed to save object of " << hash.str << ": "
                       << error;
      return;
    }
    if (reused) {
      STAT(CACHE_BLOB_REUSED);
    } else {
      new_blobs.push_back(v1->object_hash());
    }
  }

  v1->set_dep(!entry.deps.empty());
  if (!entry.deps.empty()) {
    const auto deps_path = AppendExtension(CommonPath(hash), base::kExtDeps);

    if (!base::File::Write(deps_path, entry.deps, &error)) {
      Fail();
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
      Fail();
      LOG(CACHE_ERROR) << "Failed to save split DWARF of " << hash.str << ": "
                       << error;
      return;
    }
    if (reused) {
      STAT(CACHE_BLOB_REUSED);
    } else {
      new_blobs.push_back(v1->dwo_hash());
    }
  }

  if (store_time_traces_ && !entry.time_trace.empty()) {
    if (!blobs_.Put(entry.time_trace, Path(), v1->mutable_time_trace_hash(),
                    &reused, &error, false)) {
      Fail();
      LOG(CACHE_ERROR) << "Failed to save the time trace of " << hash.str
                       << ": " << error;
      return;
    }
    if (reused) {
      STAT(CACHE_BLOB_REUSED);
    } else {
      new_blobs.push_back(v1->time_trace_hash());
    }
    v1->set_time_trace_options(entry.time_trace_options);
  }

  v1->set_size(size);
  if (!entry.warnings_hash.empty()) {
    v1->set_warnings_hash(entry.warnings_hash);
  }
  if (entry.compilation_time) {
    v1->set_compilation_time(entry.compilation_time);
  }

  if (!base::SaveBinaryToFile(manifest_path, manifest, &error)) {
    Fail();
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  NewEntries()->Append({ui64(time(nullptr)), hash, false});

  LOG(CACHE_VERBOSE) << "File is cached on path " << CommonPath(hash);
}

void FileCache::Remove(string::HandledHash hash) {
  // The index is updated only by the cleaner.
  cleaner_.Push([this, hash] { DoRemove(hash, kRemoveAttempts); });
}

void FileCache::DoRemove(string::HandledHash hash, ui32 attempts) {
  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  if (!base::File::Exists(manifest_path)) {
    return;
  }
  WriteLock lock(this, manifest_path);
  if (!lock) {
    // The entry is being read or written right now - for milliseconds.
    if (!--attempts) {
      LOG(CACHE_ERROR) << "Failed to remove the locked entry " << hash.str;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cleaner_.Push([this, hash, attempts] { DoRemove(hash, attempts); });
    return;
  }
  entries_->BeginTransaction();
  RemoveEntry(hash);
  entries_->EndTransaction();
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
    for (auto& [tag, times] : tag_times_) {
      times.erase(hash.str.string_copy());
    }
    direct_entries_.erase(hash.str.string_copy());
  }
  if (!isolated_tags_.empty()) {
    std::unique_lock<std::shared_mutex> lock(scopes_mutex_);
    for (auto& [tag, entries] : scopes_) {
      entries.erase(hash.str.string_copy());
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
    --entry_count_;
    STAT(CACHE_SIZE_CLEANED, entry_size);
  }

  return result;
}

void FileCache::DoStore(UnhandledHash orig_hash, const List<String>& headers,
                        const List<String>& preprocessed_headers,
                        const Path& current_dir, const HandledHash& hash,
                        const String& root, const List<Path>& search_dirs,
                        i64 start_time, const String& scope) {
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

  // Keep the previous variants of headers, and the entries they lead to.
  proto::Manifest old_manifest;
  List<Variant> variants;
  List<String> old_entries;
  if (base::File::Exists(manifest_path) &&
      base::LoadBinaryFromFile(manifest_path, &old_manifest) &&
      old_manifest.has_direct() &&
      old_manifest.version() == kManifestVersion) {
    variants = DirectVariants(old_manifest.direct());
    // E.g. the tag isn't isolated anymore.
    for (auto& variant : variants) {
      std::erase_if(variant.scopes, [this](const String& variant_scope) {
        return !isolated_tags_.contains(variant_scope);
      });
    }
    old_entries.assign(old_manifest.direct().entries().begin(),
                       old_manifest.direct().entries().end());
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
  ShadowChecker checker(&directory_listings_);
  const auto prefixes = SearchPrefixes(search_dirs);
  auto NormalPath = [&](const String& header) {
    return (Path(header).is_absolute() ? Path(header) : current_dir / header)
        .lexically_normal()
        .native();
  };
  Vector<String> all_header_paths;
  if (!prefixes.empty()) {
    for (const auto* list : {&headers, &preprocessed_headers}) {
      for (const auto& header : *list) {
        all_header_paths.push_back(NormalPath(header));
      }
    }
  }
  const IncluderDirs includer_dirs(all_header_paths, prefixes,
                                   &directory_listings_);
  auto collect_ignored_paths = [&](const List<String>& headers) {
    bool created = false;
    String path;
    auto Check = [&](const String& prefix, std::string_view relative) {
      struct stat st;
      if (created || !checker.IsFile(prefix, relative, &path, &st)) {
        return;
      }
      // The compiler may have missed the file, that is created during the
      // compilation.
      if (start_time && base::StatusChangeTime(st) > start_time) {
        LOG(CACHE_INFO) << "Not storing direct cache entry - " << path
                        << " is created during the compilation";
        created = true;
        return;
      }
      new_variant.ignored_paths.insert(ToManifestPath(path, current_dir, root));
    };
    for (const auto& header : headers) {
      const String header_path = NormalPath(header);
      VisitShadowingPaths(header_path, prefixes, Check);
      includer_dirs.VisitShadowingPaths(header_path, Check);
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

  const String direct_hash = base::Hexify(Immutable(hash_rope).Hash());
  DCHECK(database_);
  if (!database_->Set(direct_hash, hash) ||
      (!scope.empty() &&
       !database_->Set(ScopedRow(scope, direct_hash), hash))) {
    return;
  }

  // The most recent variant goes first, the oldest ones are dropped. The
  // variant keeps the isolated tags, that stored it before.
  if (!scope.empty()) {
    new_variant.scopes.insert(scope);
  }
  variants.remove_if([&new_variant](const Variant& variant) {
    if (variant.headers != new_variant.headers) {
      return false;
    }
    new_variant.scopes.insert(variant.scopes.begin(), variant.scopes.end());
    return true;
  });
  variants.push_front(std::move(new_variant));
  // Others and every isolated tag keep their own recent variants.
  size_t kept_variants = 0;
  HashMap<String, size_t> kept_scoped_variants;
  variants.remove_if([&](const Variant& variant) {
    bool keep = variant.scopes.empty() && kept_variants < kMaxDirectVariants;
    for (const auto& variant_scope : variant.scopes) {
      keep |= kept_scoped_variants[variant_scope] < kMaxDirectVariants;
    }
    if (keep) {
      kept_variants += variant.scopes.empty();
      for (const auto& variant_scope : variant.scopes) {
        ++kept_scoped_variants[variant_scope];
      }
    }
    return !keep;
  });
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
    for (const auto& variant_scope : variant.scopes) {
      new_variant->add_scopes(variant_scope);
    }
  }
  // The removed entries are dropped. The entries of the isolated tags and of
  // the tags with own sizes stay: the direct entry would be removed without
  // others.
  auto* direct = manifest.mutable_direct();
  direct->add_entries(hash.str.string_copy());
  size_t kept_entries = 1;
  for (const auto& entry : old_entries) {
    if (entry == hash.str.string_copy() ||
        !base::File::Exists(AppendExtension(
            CommonPath(string::Hash(Immutable(String(entry)))),
            base::kExtManifest))) {
      continue;
    }
    if (IsIsolated(entry) || IsHeld(entry)) {
      direct->add_entries(entry);
    } else if (kept_entries < kMaxDirectEntries) {
      direct->add_entries(entry);
      ++kept_entries;
    }
  }

  String error;
  if (!base::SaveBinaryToFile(manifest_path, manifest, &error)) {
    RemoveEntry(orig_hash);
    LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path << ": "
                     << error;
    return;
  }

  utime(manifest_path.c_str(), nullptr);
  NewEntries()->Append({ui64(time(nullptr)), orig_hash, false});
}

void FileCache::Clean(UniquePtr<EntryList> list) {
  entries_->BeginTransaction();
  tag_index_->BeginTransaction();
  ghosts_->BeginTransaction();
  while (auto access = list->Pop()) {
    const auto& hash = access->hash;
    if (!access->tags.empty()) {
      ApplyTags(hash, access->tags, access->time);
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
                              ui32(info.direct), JoinBlobs(info.blobs, info.time_trace, info.targets)))) {
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

  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    blobs_.RemoveUncompressed();
  }

  if (Clock::now() >= next_sizes_report_) {
    ReportSizes();
  }
}

void FileCache::Maintain() {
  // The rows of the direct cache, that lead to the removed entries: the
  // database would only grow. The entries, that aren't indexed yet, have the
  // manifests.
  HashSet<String> indexed;
  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    indexed.insert(hash);
  }
  const ui64 removed_rows = database_->DeleteIf(
      [this, &indexed](const String& key, const String& value) {
        if (key.starts_with("seen:") || key.starts_with("source:") ||
            indexed.contains(value)) {
          return false;
        }
        const string::Hash hash{Immutable(value)};
        return !base::File::Exists(
            AppendExtension(CommonPath(hash), base::kExtManifest));
      });
  if (removed_rows) {
    LOG(CACHE_VERBOSE) << "Removed " << removed_rows
                       << " rows of the direct cache";
  }

  entries_->BeginTransaction();
  tag_index_->BeginTransaction();
  ghosts_->BeginTransaction();
  TrimQuotas();
  Evict();
  RemoveUnreachableDirectEntries();
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

  ui64 time_traces_size = 0;
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (auto it = time_trace_blobs_.begin(); it != time_trace_blobs_.end();) {
      if (!blobs_.Refs(*it)) {
        it = time_trace_blobs_.erase(it);
        continue;
      }
      time_traces_size += blobs_.Size(*it);
      ++it;
    }
  }
  stats.Set(M::CACHE_TIME_TRACES_SIZE, time_traces_size);

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
  // The hits of the isolated tags shouldn't depend on the time since their
  // last use: they never uncompress.
  HashSet<String> kept;
  {
    std::shared_lock<std::shared_mutex> scopes_lock(scopes_mutex_);
    for (const auto& [tag, entries] : scopes_) {
      for (const auto& entry : entries) {
        auto it = entry_blobs_.find(entry);
        if (it != entry_blobs_.end()) {
          kept.insert(it->second.begin(), it->second.end());
        }
      }
    }
  }
  return blobs_.CompressUnused(now - std::min(now, compress_after_),
                               kMaxCompressedSizePerPass,
                               Clock::now() + kMaxTimePerPass, kept);
}

bool FileCache::LoadEntryInfo(string::Hash hash, EntryInfo* info) const {
  const auto manifest_path =
      AppendExtension(CommonPath(hash), base::kExtManifest);
  proto::Manifest manifest;
  if (!base::LoadBinaryFromFile(manifest_path, &manifest) ||
      manifest.version() != kManifestVersion ||
      (!manifest.has_v1() && !manifest.has_direct())) {
    return false;
  }

  info->direct = manifest.has_direct();
  info->targets.assign(manifest.direct().entries().begin(),
                       manifest.direct().entries().end());
  info->size = manifest.v1().size() + base::File::Size(manifest_path);
  if (manifest.v1().obj()) {
    info->blobs.push_back(manifest.v1().object_hash());
    info->object = manifest.v1().object_hash();
  }
  info->compilation_time = manifest.v1().compilation_time();
  if (manifest.v1().dwo()) {
    info->blobs.push_back(manifest.v1().dwo_hash());
  }
  if (manifest.v1().has_time_trace_hash()) {
    info->time_trace = manifest.v1().time_trace_hash();
    info->blobs.push_back(info->time_trace);
  }
  return true;
}

void FileCache::AddBlobRefs(string::Hash hash, const EntryInfo& info,
                            ui64 access_time) {
  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  if (info.direct) {
    direct_entries_[hash.str.string_copy()] = info.targets;
  }
  // The new references go first: the blobs may be the same.
  for (const auto& blob : info.blobs) {
    blobs_.AddRef(blob, access_time);
  }
  if (!info.time_trace.empty()) {
    time_trace_blobs_.insert(info.time_trace);
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
    info.blobs = SplitBlobs(std::get<SQLite::BLOBS>(entry), &info.time_trace,
                            &info.targets);
    AddBlobRefs(string::Hash(Immutable(hash)), info,
                std::get<SQLite::MTIME>(entry));
    cache_size_ += std::get<SQLite::SIZE>(entry);
    ++entry_count_;
  }

  // The tags of the removed entries may stay after a crash. The tags of the
  // older revisions - or of the older daemons - have no times: they get the
  // times of the entries.
  {
    auto all_tags = tag_index_->GetAll();
    auto all_times = tag_index_->GetAllTimes();
    tag_index_->BeginTransaction();
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (auto& [entry, tags] : all_tags) {
      SQLite::Value value;
      if (!entries_->Get(entry, &value)) {
        tag_index_->Delete(entry);
        continue;
      }
      for (const auto& tag : tags) {
        auto& time = tag_times_[tag][entry];
        auto times_it = all_times.find(tag);
        if (times_it != all_times.end() && times_it->second.contains(entry)) {
          time = times_it->second[entry];
          times_it->second.erase(entry);
        } else {
          time = std::get<SQLite::MTIME>(value);
          tag_index_->SetTime(entry, tag, time);
        }
      }
      entry_tags_[entry] = std::move(tags);
    }
    // The times of the tags, that the entries don't have anymore.
    for (const auto& [tag, times] : all_times) {
      for (const auto& [entry, time] : times) {
        tag_index_->DeleteTime(entry, tag);
      }
    }
    tag_index_->EndTransaction();
  }
  // The tags, that are added since the start, stay.
  for (const auto& tag : isolated_tags_) {
    UpdateScope(tag, true);
  }

  // E.g. the blobs of the entries, that weren't stored because of a crash.
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    blobs_.RemoveUnreferenced(start_time);
  }

  ReportSizes();
}

void FileCache::RemoveTimeTraces() {
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    if (time_trace_blobs_.empty()) {
      return;
    }
  }

  LOG(CACHE_INFO) << "Removing the time traces from the cache " << path_;
  entries_->BeginTransaction();
  for (auto [hash_str, value] : entries_->GetInEvictionOrder()) {
    String time_trace;
    SplitBlobs(std::get<SQLite::BLOBS>(value), &time_trace);
    if (time_trace.empty()) {
      continue;
    }

    const string::Hash hash{Immutable(hash_str)};
    const auto manifest_path =
        AppendExtension(CommonPath(hash), base::kExtManifest);
    // The entry is being written right now - the next start removes its
    // trace.
    WriteLock lock(this, manifest_path);
    proto::Manifest manifest;
    if (!lock || !base::LoadBinaryFromFile(manifest_path, &manifest) ||
        !manifest.has_v1()) {
      continue;
    }
    manifest.mutable_v1()->clear_time_trace_hash();
    manifest.mutable_v1()->clear_time_trace_options();
    String error;
    if (!base::SaveBinaryToFile(manifest_path, manifest, &error)) {
      LOG(CACHE_ERROR) << "Failed to save manifest to " << manifest_path
                       << ": " << error;
      continue;
    }

    EntryInfo info;
    if (!LoadEntryInfo(hash, &info)) {
      continue;
    }
    // The trace is removed with its last reference.
    AddBlobRefs(hash, info, std::get<SQLite::MTIME>(value));
    cache_size_ += info.size;
    cache_size_ -= std::get<SQLite::SIZE>(value);
    std::get<SQLite::SIZE>(value) = info.size;
    std::get<SQLite::BLOBS>(value) = JoinBlobs(info.blobs, info.time_trace, info.targets);
    if (!entries_->Set(hash_str, value)) {
      LOG(CACHE_ERROR) << "Failed to update the index for " << hash_str;
    }
  }
  entries_->EndTransaction();

  ReportSizes();
}

void FileCache::RemoveUnreachableDirectEntries() {
  HashSet<String> indexed;
  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    if (!std::get<SQLite::DIRECT>(value)) {
      indexed.insert(hash);
    }
  }
  // The entries, that aren't indexed yet, have the manifests.
  auto Reachable = [&](const Vector<String>& targets) {
    for (const auto& target : targets) {
      if (indexed.contains(target) ||
          base::File::Exists(
              AppendExtension(CommonPath(string::Hash(Immutable(target))),
                              base::kExtManifest))) {
        return true;
      }
    }
    return false;
  };

  List<String> unreachable;
  {
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    for (const auto& [hash, targets] : direct_entries_) {
      if (!Reachable(targets)) {
        unreachable.push_back(hash);
      }
    }
  }
  if (unreachable.empty()) {
    return;
  }

  ui32 removed = 0;
  for (const auto& hash_str : unreachable) {
    const string::Hash hash{Immutable(hash_str)};
    WriteLock lock(this,
                   AppendExtension(CommonPath(hash), base::kExtManifest));
    // The manifest may lead to a new entry already - not applied to the
    // index yet.
    EntryInfo info;
    if (lock && (!LoadEntryInfo(hash, &info) || !Reachable(info.targets))) {
      RemoveEntry(hash);
      ++removed;
    }
  }
  LOG(CACHE_INFO) << "Removed " << removed
                  << " direct entries, that lead to no entries";
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

void FileCache::TrimQuotas() {
  if (tag_quotas_.empty()) {
    return;
  }
  HashMap<String, ui64> sizes;
  for (const auto& [hash, value] : entries_->GetInEvictionOrder()) {
    sizes.emplace(hash, std::get<SQLite::SIZE>(value));
  }

  std::lock_guard<Mutex> lock(blob_refs_mutex_);
  // The direct entries count for the tags of the entries, that they lead to.
  HashMap<String, Vector<String>> leading;
  for (const auto& [entry, targets] : direct_entries_) {
    if (sizes.contains(entry)) {
      for (const auto& target : targets) {
        leading[target].push_back(entry);
      }
    }
  }

  for (const auto& [tag, quota] : tag_quotas_) {
    if (!quota) {
      continue;
    }

    // The entries of the tag - from the oldest use by the tag.
    Vector<Pair<ui64, String>> tagged;
    const auto& times = tag_times_[tag];
    for (const auto& [entry, tags] : entry_tags_) {
      if (sizes.contains(entry) &&
          std::binary_search(tags.begin(), tags.end(), tag)) {
        auto it = times.find(entry);
        tagged.emplace_back(it == times.end() ? 0 : it->second, entry);
      }
    }
    std::sort(tagged.begin(), tagged.end());

    // Every blob and every direct entry counts once.
    ui64 usage = 0;
    HashMap<String, ui32> blob_refs, direct_refs;
    auto Count = [&](const String& entry, bool add) {
      const auto Change = [&](ui64 size) {
        usage = add ? usage + size : usage - std::min(usage, size);
      };
      Change(sizes[entry]);
      if (auto it = entry_blobs_.find(entry); it != entry_blobs_.end()) {
        for (const auto& blob : it->second) {
          auto& refs = blob_refs[blob];
          if (add ? !refs++ : !--refs) {
            Change(blobs_.Size(blob));
          }
        }
      }
      if (auto it = leading.find(entry); it != leading.end()) {
        for (const auto& direct : it->second) {
          auto& refs = direct_refs[direct];
          if (add ? !refs++ : !--refs) {
            Change(sizes[direct]);
          }
        }
      }
    };
    for (const auto& [time, entry] : tagged) {
      Count(entry, true);
    }

    // The entries keep their other tags.
    ui32 untagged = 0;
    for (auto it = tagged.begin(); it != tagged.end() && usage > quota;
         ++it, ++untagged) {
      const String& entry = it->second;
      Count(entry, false);
      auto& tags = entry_tags_[entry];
      tags.erase(std::lower_bound(tags.begin(), tags.end(), tag));
      if (tags.empty()) {
        entry_tags_.erase(entry);
        tag_index_->Delete(entry);
      } else {
        tag_index_->Set(entry, tags);
        tag_index_->DeleteTime(entry, tag);
      }
      tag_times_[tag].erase(entry);
      if (isolated_tags_.contains(tag)) {
        std::unique_lock<std::shared_mutex> scopes_lock(scopes_mutex_);
        scopes_[tag].erase(entry);
      }
    }
    if (untagged) {
      LOG(CACHE_INFO) << "The tag " << tag << " left " << untagged
                      << " entries to fit into " << quota << " bytes";
    }
  }
}

void FileCache::Evict() {
  if (max_size_ == UNLIMITED && !min_free_percent_) {
    return;
  }

  // The entries, which outputs are shared with build directories, are free:
  // their removal doesn't free the disk space. They are likely to be used
  // soon, so they are never removed.
  // The entries of the tags with own sizes are held: they don't count in the
  // size of the cache, and they aren't removed.
  auto entries = entries_->GetInEvictionOrder();
  const HashSet<String> held = HeldEntries();
  ui64 own_size = 0;
  Vector<ui64> own_sizes;
  own_sizes.reserve(entries.size());
  for (const auto& [hash_str, entry] : entries) {
    const string::Hash hash{Immutable(hash_str)};
    const ui64 size = std::get<SQLite::SIZE>(entry);
    if (held.contains(hash_str)) {
      own_sizes.push_back(0);
      continue;
    }
    own_sizes.push_back(HasSharedBlobs(hash) ? 0 : size);
    own_size += size;
  }
  {
    // Every blob counts once.
    std::lock_guard<Mutex> lock(blob_refs_mutex_);
    BlobStore::Sizes sizes;
    blobs_.GetSizes(&sizes);
    HashSet<String> held_blobs;
    for (const auto& entry : held) {
      auto it = entry_blobs_.find(entry);
      if (it != entry_blobs_.end()) {
        held_blobs.insert(it->second.begin(), it->second.end());
      }
    }
    for (const auto& blob : held_blobs) {
      if (!blobs_.IsShared(blob)) {
        sizes.own -= std::min(sizes.own, blobs_.Size(blob));
      }
    }
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
    // The direct entries are small: their removal frees almost nothing on the
    // disk, but the entries, that they lead to, aren't found without the
    // preprocessing anymore - e.g. the protected ones.
    if ((max_size_ == UNLIMITED || own_size <= target_size) &&
        std::get<SQLite::DIRECT>(entry)) {
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
