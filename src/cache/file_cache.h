#pragma once

#include <base/const_string.h>
#include <base/locked_list.h>
#include <base/thread_pool.h>
#include <cache/blob_store.h>
#include <cache/database_kv.h>
#include <cache/database_sqlite.h>
#include <cache/file_hash_cache.h>
#include <cache/manifest.pb.h>
#include <cache/tag_index.h>

#include <base/test_forward.h>

namespace dist_clang {
namespace cache {

FORWARD_TEST(FileCacheTest, DoubleLocks);
FORWARD_TEST(FileCacheTest, ExceedCacheSize);
FORWARD_TEST(FileCacheTest, EntryWithoutHashes);
FORWARD_TEST(FileCacheTest, DetectBrokenFiles);
FORWARD_TEST(FileCacheTest, EvictNeverHitEntriesFirst);
FORWARD_TEST(FileCacheTest, ExceedCacheSize_Sync);
FORWARD_TEST(FileCacheTest, LockNonExistentFile);
FORWARD_TEST(FileCacheTest, RemoveEntry);
FORWARD_TEST(FileCacheTest, TagStatsDontWaitForMaintenance);
FORWARD_TEST(FileCacheTest, Format);
FORWARD_TEST(FileCacheTest, TagStats);
FORWARD_TEST(FileCacheTest, ForgetTag);
FORWARD_TEST(FileCacheTest, DeduplicateOutputs);
FORWARD_TEST(FileCacheTest, RemoveUnreferencedBlobs);
FORWARD_TEST(FileCacheTest, CompressColdEntries);
FORWARD_TEST(FileCacheTest, RoundTripCompressedEntry);
FORWARD_TEST(FileCacheTest, RemoveEntryInBackground);
FORWARD_TEST(FileCacheTest, RestoreEntryWithMissingFile);
FORWARD_TEST(FileCacheTest, CheckFilesOnlyAfterCrash);

enum ExtraFileType {
  SANITIZE_BLACKLIST = 0,
  KEY_FILES = 1,
  // Hashes of files, which contents affect the result, e.g. profiles.
};

using ExtraFiles = HashMap<ExtraFileType, Immutable>;

namespace string {

#define DEFINE_STRING_TYPE(type)                                          \
  struct type {                                                           \
    type() = default;                                                     \
    explicit type(Immutable str) : str(str) {}                            \
    operator Immutable() const { return str; }                            \
    bool operator==(const type& other) const { return str == other.str; } \
                                                                          \
    /* TODO: remove this */                                               \
    explicit type(const String& str) : str(str) {}                        \
                                                                          \
    Immutable str;                                                        \
  }

#define DEFINE_STRING_SUBTYPE(type, base_type)           \
  struct type : public base_type {                       \
    type() = default;                                    \
    explicit type(Immutable str) : base_type(str) {}     \
                                                         \
    /* TODO: remove this */                              \
    explicit type(const String& str) : base_type(str) {} \
  }

DEFINE_STRING_TYPE(HandledSource);
DEFINE_STRING_TYPE(UnhandledSource);
DEFINE_STRING_TYPE(Hash);
DEFINE_STRING_SUBTYPE(HandledHash, Hash);
DEFINE_STRING_SUBTYPE(UnhandledHash, Hash);
DEFINE_STRING_TYPE(CommandLine);
DEFINE_STRING_TYPE(Version);

#undef DEFINE_STRING_SUBTYPE
#undef DEFINE_STRING_TYPE

}  // namespace string
}  // namespace cache
}  // namespace dist_clang

namespace std {

template <>
struct hash<dist_clang::cache::string::Hash> {
 public:
  size_t operator()(dist_clang::cache::string::Hash value) const {
    return std::hash<dist_clang::Immutable>()(value.str);
  }
};

}  // namespace std

namespace dist_clang {
namespace cache {

class FileCache {
 public:
  enum : ui64 {
    UNLIMITED = 0,
  };

  // The format of the cache on disk - see |proto::Format|. Bump the generation
  // for the changes, that the older daemons can't work with, or that may give
  // wrong results with them: the layout of the files, the semantics of the
  // manifests and of the databases. Bump the revision for the compatible
  // changes, e.g. new optional fields, that the older daemons ignore.
  //
  // Nothing is migrated: the entries of other versions are just removed.
  //
  // Generation 1: the cache before the generations - with the migrations of
  //               the old manifests, and the outputs beside the manifests.
  // Generation 2: the outputs only in the blob store, the blobs of the
  //               entries in the index, no Snappy.
  enum : ui32 { kGeneration = 2, kRevision = 1 };

  // Maximal number of the header sets per one source in the direct cache.
  enum : size_t { kMaxDirectVariants = 8 };

  struct Entry {
    Immutable object;
    Immutable deps;
    Immutable stderr;
    Immutable dwo;

    // With linked outputs the object and split DWARF files are passed by
    // paths instead of contents: the produced files for |Store()| and the
    // files in the cache from |Find()|.
    Path object_path;
    Path dwo_path;

    // Hashes of the linked files from |Find()|.
    String object_hash;
    String dwo_hash;

    // Hash of the warning flags of the compilation.
    String warnings_hash;

    // The key of the found entry - from |Find()|.
    String handled_hash;
  };

  // With |link_outputs| the outputs are stored uncompressed, and are linked
  // into and out of the cache without copying, where possible.
  FileCache(const Path& path, ui64 size, proto::Compression compression,
            bool store_index, bool link_outputs = false);
  explicit FileCache(const Path& path);
  ~FileCache();

  bool Run(ui64 clean_period);
  // |clean_period| is in seconds.

  // Waits for the work of the cleaner, that is queued now - e.g. for the check
  // of the files after the start.
  void WaitForCleaner();

  // Stops the periodic work, applies the recent accesses to the index, and
  // marks the normal stop: the files aren't checked at the next start.
  void Close();

  // Removes entries, while the disk has less free space - besides the size
  // limit. Only the entries, that aren't linked with build directories, count.
  inline void SetMinFreePercent(ui32 percent) { min_free_percent_ = percent; }

  // With linked outputs: compresses the outputs, that aren't linked with build
  // directories anymore, and weren't used for |seconds|. Zero disables it.
  inline void SetCompressAfter(ui64 seconds) { compress_after_ = seconds; }

  static string::HandledHash Hash(string::HandledSource code,
                                  const ExtraFiles& extra_files,
                                  string::CommandLine command_line,
                                  string::Version version);

  static string::UnhandledHash Hash(string::UnhandledSource code,
                                    const ExtraFiles& extra_files,
                                    string::CommandLine command_line,
                                    string::Version version);

  // The headers under the source |root| are stored relative to it - so that
  // the same sources from different locations are found.
  //
  // |search_dirs| are the absolute normal paths of the directories, where the
  // compiler searches the headers, in the order of the search. The entry isn't
  // found, if a new file appears in them, that would be found instead of one
  // of the headers.
  bool Find(string::UnhandledSource code, const ExtraFiles& extra_files,
            string::CommandLine command_line, string::Version version,
            const Path& current_dir, Entry* entry,
            const String& root = String(),
            const List<Path>& search_dirs = List<Path>()) const;

  bool Find(string::HandledHash hash, Entry* entry) const;

  // The files in |search_dirs|, that could be found instead of the headers,
  // mustn't be created after |start_time| - the start of the compilation in
  // nanoseconds: the compiler may have missed them.
  void Store(string::UnhandledSource code, const ExtraFiles& extra_files,
             string::CommandLine command_line, string::Version version,
             const List<String>& headers,
             const List<String>& preprocessed_headers, const Path& current_dir,
             string::HandledHash hash, const String& root = String(),
             const List<Path>& search_dirs = List<Path>(),
             i64 start_time = 0);

  void Store(string::HandledHash hash, Entry entry);

  // Removes the entry in background - e.g. if its result is wrong. The direct
  // entries, that refer to it, aren't found anymore.
  void Remove(string::HandledHash hash);

  // Returns the paths, where a new file would be found by the compiler instead
  // of |header|: the same relative path in the |search_dirs| before the
  // directory of the header. All paths are absolute and normal. The include
  // directive isn't known, so not all of them are real candidates.
  static List<Path> ShadowingPaths(const Path& header,
                                   const List<Path>& search_dirs);

  // Tags the entry, e.g. with the branch of the sources, that used it. The
  // tags are kept by the cleaner, until the entry is removed.
  void AddTags(string::HandledHash hash, const Vector<String>& tags);

  struct TagStats {
    String tag;  // Empty for the entries without tags.
    ui64 entries = 0, size = 0;
    // The part of the size, that is linked with build directories: it takes
    // no space in the cache, and isn't removed with the tag.
    ui64 linked_size = 0;
    // Only for this tag.
    ui64 exclusive_entries = 0, exclusive_size = 0;
    // The size, that |ForgetTag()| frees now.
    ui64 freeable_size = 0;
  };
  struct TagOverlap {
    String first, second;
    ui64 size = 0;  // Of the entries and the outputs, used by both tags.
  };
  struct ForgetResult {
    ui64 removed_entries = 0, freed_size = 0;
    // Kept: used by other tags, or linked with build directories.
    ui64 shared_entries = 0, linked_entries = 0;
  };

  using TagStatsCallback =
      Fn<void(const List<TagStats>&, const List<TagOverlap>&)>;
  // Tells, if the result is still needed - e.g. the client is still waiting.
  using NeededFn = Fn<bool()>;

  // Calls back from the cleaner with the statistics of all tags. The sizes
  // count every output once. The requests, that wait at the same time, get
  // the same statistics; they aren't computed, if nobody needs them anymore.
  bool GetTagStats(TagStatsCallback callback,
                   NeededFn needed = [] { return true; });

  // Removes the entries, which only have the |tag|, from the cache - except
  // the ones, that are linked with build directories. Removes the |tag| from
  // other entries. The empty |tag| removes the entries without tags. With
  // |dry_run| only counts them. Calls back from the cleaner, if it's still
  // |needed|.
  bool ForgetTag(const String& tag, bool dry_run,
                 Fn<void(const ForgetResult&)> callback,
                 NeededFn needed = [] { return true; });

  // Remembers the |key|. Returns |true| if it's new.
  bool MarkSeen(const String& key);

  // Remembers the last version of the source, compiled with the same flags,
  // under |key|. Returns |true| if the previous version is different - i.e.
  // the source is edited.
  bool ReplaceLastSource(const String& key, string::UnhandledHash hash);

 private:
  FRIEND_TEST(FileCacheTest, DoubleLocks);
  FRIEND_TEST(FileCacheTest, ExceedCacheSize);
  FRIEND_TEST(FileCacheTest, EntryWithoutHashes);
  FRIEND_TEST(FileCacheTest, DetectBrokenFiles);
  FRIEND_TEST(FileCacheTest, EvictNeverHitEntriesFirst);
  FRIEND_TEST(FileCacheTest, LockNonExistentFile);
  FRIEND_TEST(FileCacheTest, RemoveEntry);
  FRIEND_TEST(FileCacheTest, TagStatsDontWaitForMaintenance);
  FRIEND_TEST(FileCacheTest, Format);
  FRIEND_TEST(FileCacheTest, TagStats);
  FRIEND_TEST(FileCacheTest, ForgetTag);
  FRIEND_TEST(FileCacheTest, DeduplicateOutputs);
  FRIEND_TEST(FileCacheTest, RemoveUnreferencedBlobs);
  FRIEND_TEST(FileCacheTest, CompressColdEntries);
  FRIEND_TEST(FileCacheTest, RoundTripCompressedEntry);
  FRIEND_TEST(FileCacheTest, RemoveEntryInBackground);
  FRIEND_TEST(FileCacheTest, RestoreEntryWithMissingFile);
  FRIEND_TEST(FileCacheTest, CheckFilesOnlyAfterCrash);
  FRIEND_TEST(FileCacheTest, ReportSizes);

  enum : ui32 { kManifestVersion = 2 };

  class ReadLock {
   public:
    ReadLock(const FileCache* WEAK_PTR cache, const String& path);
    ~ReadLock() { Unlock(); }

    inline operator bool() const { return locked_; }
    void Unlock();

   private:
    const FileCache* cache_;
    const String path_;
    bool locked_ = false;
  };

  class WriteLock {
   public:
    WriteLock(const FileCache* WEAK_PTR cache, const String& path);
    ~WriteLock() { Unlock(); }

    inline operator bool() const { return locked_; }
    void Unlock();

   private:
    const FileCache* cache_;
    const String path_;
    bool locked_ = false;
  };

  friend class ReadLock;
  friend class WriteLock;

  inline Path SecondPath(string::Hash hash) const {
    DCHECK(hash.str.size() >= 2);
    return path_ / String(1, hash.str[0]) / String(1, hash.str[1]);
  }

  inline Path CommonPath(string::Hash hash) const {
    return SecondPath(hash) / hash.str.string_copy();
  }

  void DoStore(string::UnhandledHash orig_hash, const List<String>& headers,
               const List<String>& preprocessed_headers,
               const Path& current_dir, const string::HandledHash& hash,
               const String& root, const List<Path>& search_dirs,
               i64 start_time);

  // Checks, that no new files appeared in the search directories, which would
  // be found instead of the |headers|.
  bool IsShadowed(const List<String>& headers,
                  const HashSet<String>& ignored_paths,
                  const Path& current_dir, const String& root,
                  const List<Path>& search_dirs) const;

  struct Access {
    ui64 time;
    string::Hash hash;
    bool hit;  // |false| if the entry is just stored.
    Vector<String> tags = {};  // Only adds the tags, if not empty.
  };
  using EntryList = base::LockedList<Access>;
  using EntryListDeleter = Fn<void(EntryList* list)>;

  // Reads the small file of the entry, and checks its hash.
  bool FindFile(const Path& cache_path, const String& expected_hash,
                Immutable* content, ui64* size) const;

  // What the index keeps about the entry - from its manifest.
  struct EntryInfo {
    ui64 size = 0;  // With the manifest, without the blobs.
    bool direct = false;
    Vector<String> blobs;
  };
  // Returns |false| for a broken entry, or of another version.
  bool LoadEntryInfo(string::Hash hash, EntryInfo* info) const;

  // Counts the references of the entry to the blobs - in the cleaner.
  void AddBlobRefs(string::Hash hash, const EntryInfo& info,
                   ui64 access_time);
  void RemoveBlobRefs(string::Hash hash);
  // Checks the files of all entries after a crash: removes the files, that
  // are left by it before |start_time|, removes the entries of other
  // versions, and adds the missing ones to the index.
  void ScanEntries(ui64 start_time);

  // Counts the references of all entries - after the start.
  void CountBlobRefs(ui64 start_time);

  // The work with the tags - in the cleaner.
  void ApplyTags(string::Hash hash, const Vector<String>& tags);
  void ComputeTagStats(List<TagStats>* stats, List<TagOverlap>* overlaps);
  ForgetResult DoForgetTag(const String& tag, bool dry_run);

  // Checks the format of the cache on disk, and resets the cache of an older
  // generation. Returns |false| if the cache can't be used.
  bool CheckFormat();

  // The size of the files, that aren't the entries: the databases, etc.
  ui64 MetadataSize() const;

  bool RemoveEntry(string::Hash hash);
  // Returns |false| only if some part of entry can't be physically removed.

  // Applies the accesses to the index - quickly, since the requests of the
  // tags wait for it.
  void Clean(UniquePtr<EntryList> list);
  // Evicts and compresses the entries - periodically, with a limit of time.
  void Maintain();

  // Updates the statistics of the size of the cache - in the cleaner.
  void ReportSizes();

  // Removes entries, if the cache is too large, or the disk is too full.
  void Evict();

  // If any blob of the entry is hard-linked with other places.
  bool HasSharedBlobs(string::Hash hash);
  // Size of the blobs, that are used only by the entry.
  ui64 GetOwnBlobsSize(string::Hash hash);

  // Compresses the unused outputs - see |SetCompressAfter()|. Returns the
  // number of compressed blobs.
  ui32 CompressColdEntries();

  mutable std::mutex locks_mutex_;
  mutable HashMap<String, ui32> read_locks_;
  mutable HashSet<String> write_locks_;

  const Path path_;
  bool store_index_, link_outputs_;
  UniquePtr<SQLiteKV> database_;
  mutable FileHashCache file_hashes_;
  UniquePtr<SQLite> entries_;

  BlobStore blobs_;
  // The blobs of the entries. Used by the cleaner - and by the failed stores.
  Mutex blob_refs_mutex_;
  HashMap<String, Vector<String>> entry_blobs_;
  // The manifests of the direct cache - they aren't tagged.
  HashSet<String> direct_entries_;
  // The tags of the entries - sorted. Used by the cleaner too.
  UniquePtr<TagIndex> tag_index_;
  HashMap<String, Vector<String>> entry_tags_;

  ui64 max_size_;
  // The size of the entries without the blobs, and their number - the failed
  // stores remove the entries too.
  Atomic<ui64> cache_size_ = {0u}, entry_count_ = {0u};
  Clock::time_point next_sizes_report_;
  // The accesses are applied to the index at least so often, in seconds - the
  // statistics show them.
  ui64 flush_period_ = 10;
  ui32 min_free_percent_ = 0;
  ui64 compress_after_ = 0;
  const EntryListDeleter new_entries_deleter_ = [this](EntryList* list) {
    auto task = [this, list] { Clean(UniquePtr<EntryList>(list)); };
    cleaner_.Push(task);
  };
  SharedPtr<EntryList> new_entries_;

  // Removes the file "running" after the cleaner - at the normal stop.
  struct RunningMarker {
    ~RunningMarker();
    Path path;
  } running_marker_;

  base::ThreadPool cleaner_{base::ThreadPool::TaskQueue::UNLIMITED, 1};

  // The requests of the statistics of the tags, that wait for it.
  Mutex tag_stats_mutex_;
  List<Pair<TagStatsCallback, NeededFn>> tag_stats_requests_;

  UniquePtr<base::WorkerPool> resetter_{new base::WorkerPool(true)};
  // Resets |new_entries_| periodically, and starts the maintenance.
};

}  // namespace cache
}  // namespace dist_clang
