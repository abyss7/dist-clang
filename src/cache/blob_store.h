#pragma once

#include <base/const_string.h>
#include <base/types.h>
#include <cache/manifest.pb.h>

struct stat;

namespace dist_clang {
namespace cache {

// Stores the outputs of the cache entries by the hashes of their contents -
// so that the same output of several entries takes the space once.
//
// A blob is either uncompressed - and may be linked with build directories -
// or compressed with zstd. The blobs are put and got from any thread; the
// references from the entries are counted under the lock of the owner - see
// |FileCache::blob_refs_mutex_|: by the cleaner, and by the failed stores.
class BlobStore {
 public:
  // With |link| the files are put and got by linking, where possible, and
  // aren't compressed. Otherwise the blobs are compressed, unless the
  // |compression| is NONE.
  BlobStore(const Path& root, proto::Compression compression, bool link);

  // Puts the |content|, or the file |path|, if the |content| is empty. Sets
  // the |hash| of the contents. Reuses the existing blob with the same hash.
  // The blobs, that aren't |linkable| - only read - are compressed at once.
  bool Put(Immutable content, const Path& path, String* hash, bool* reused,
           String* error, bool linkable = true) const THREAD_SAFE;

  // Finds the blob: sets |path| to the uncompressed file, if it can be linked
  // - and |path| isn't null - or reads the |content| otherwise. Checks the
  // hash of the contents: the broken blob is removed. Adds the size of the
  // file to |size|.
  bool Get(const String& hash, Immutable* content, Path* path,
           ui64* size) const THREAD_SAFE;

  // If the file of the linked blob is the same, as at its last check.
  bool IsVerified(const String& hash) const THREAD_SAFE;
  // The blob is restored by the daemon itself - its links and times are
  // changed, but not the contents. Call, if it |IsVerified()| just before.
  void Restored(const String& hash) const THREAD_SAFE;

  // Removes the new blob of a failed store: nothing refers to it.
  void Discard(const String& hash) const THREAD_SAFE;

  // The blob is used by an entry, that may be not counted yet - e.g. by a hit
  // during the scan after a crash: it isn't removed as unreferenced.
  void MarkUsed(const String& hash) const THREAD_SAFE;

  // The methods below aren't thread-safe: the caller holds the lock of the
  // references - see |FileCache::blob_refs_mutex_|.

  // Counts the reference from an entry, that was used at |access_time|.
  void AddRef(const String& hash, ui64 access_time);
  void Touch(const String& hash, ui64 access_time);
  // Removes the blob without references.
  void RemoveRef(const String& hash);

  // Removes the compressed versions of the blobs, that are uncompressed again
  // by |Get()|.
  void RemoveUncompressed();

  // Removes the blobs without references, that are created before |time| -
  // e.g. after a crash. Call after all references are added.
  ui32 RemoveUnreferenced(ui64 time);

  // Compresses the blobs, that aren't linked with other places, and weren't
  // used since |time| - except the |kept| ones. Stops after |max_size| bytes,
  // or at |deadline|. Returns the number of the compressed blobs.
  ui32 CompressUnused(ui64 time, ui64 max_size,
                      Clock::time_point deadline = Clock::time_point::max(),
                      const HashSet<String>& kept = HashSet<String>());

  // The size on the disk, and if the blob is linked with other places - e.g.
  // with build directories, so its removal doesn't free the space.
  ui64 Size(const String& hash) const;
  bool IsShared(const String& hash) const;
  ui32 Refs(const String& hash) const;

  struct Sizes {
    // The blobs, that aren't and are linked with other places.
    ui64 own = 0, shared = 0;
    // The space of the copies, that aren't needed: of the blobs of several
    // entries, and of the blobs linked with several other places - e.g. with
    // the build directories of several working copies.
    ui64 deduplicated = 0, linked = 0;
  };
  // Adds the sizes of all blobs.
  void GetSizes(Sizes* sizes) const;

  Path RawPath(const String& hash) const;
  Path PackedPath(const String& hash) const;

 private:
  struct Blob {
    ui32 refs = 0;
    ui64 access_time = 0;
  };

  // The file of a linked blob, as it was, when its contents were checked: the
  // modification through any link changes it. Not the time of the change of
  // the status: the build directories add and remove the links to the blob
  // all the time.
  struct Stamp {
    ui64 device, inode, size;
    i64 mtime_ns;
    bool operator==(const Stamp&) const = default;
  };
  static Stamp StampOf(const struct stat& st);
  // Remembers the checked contents of the file with the stat |st|.
  void Remember(const String& hash, const struct stat& st) const;

  void Delete(const String& hash);

  const Path root_;
  const proto::Compression compression_;
  const bool link_;

  // The blobs, that are reused, until |RemoveUnreferenced()|: the entries,
  // that refer to them, may be not counted yet.
  mutable Mutex reused_mutex_;
  mutable HashSet<String> reused_;
  bool track_reused_ = true;

  // The blobs, that are uncompressed by |Get()|, until the cleaner removes
  // their compressed versions.
  mutable Mutex unpacked_mutex_;
  mutable HashSet<String> unpacked_;

  HashMap<String, Blob> blobs_;

  // The linked blobs, which contents are checked already: hashing them at
  // every hit would read tens of megabytes of the debug info.
  mutable Mutex verified_mutex_;
  mutable HashMap<String, Stamp> verified_;
};

}  // namespace cache
}  // namespace dist_clang
