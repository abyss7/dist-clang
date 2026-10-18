#pragma once

#include <base/const_string.h>
#include <base/types.h>
#include <cache/manifest.pb.h>

namespace dist_clang {
namespace cache {

// Stores the outputs of the cache entries by the hashes of their contents -
// so that the same output of several entries takes the space once.
//
// A blob is either uncompressed - and may be linked with build directories -
// or compressed with zstd. The blobs are put and got from any thread; the
// references from the entries are counted only by one thread - the cleaner.
class BlobStore {
 public:
  // With |link| the files are put and got by linking, where possible, and
  // aren't compressed. Otherwise the blobs are compressed, unless the
  // |compression| is NONE.
  BlobStore(const Path& root, proto::Compression compression, bool link);

  // Puts the |content|, or the file |path|, if the |content| is empty. Sets
  // the |hash| of the contents. Reuses the existing blob with the same hash.
  bool Put(Immutable content, const Path& path, String* hash, bool* reused,
           String* error) const THREAD_SAFE;

  // Finds the blob: sets |path| to the uncompressed file, if it can be linked
  // - and |path| isn't null - or reads the |content| otherwise. Checks the
  // hash of the contents: the broken blob is removed. Adds the size of the
  // file to |size|.
  bool Get(const String& hash, Immutable* content, Path* path,
           ui64* size) const THREAD_SAFE;

  // Removes the new blob of a failed store: nothing refers to it.
  void Discard(const String& hash) const THREAD_SAFE;

  // The blob is used by an entry, that may be not counted yet - e.g. by a hit
  // during the scan after a crash: it isn't removed as unreferenced.
  void MarkUsed(const String& hash) const THREAD_SAFE;

  // The methods below are called only by the cleaner.

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
  // used since |time|. Stops after |max_size| bytes, or at |deadline|.
  // Returns the number of the compressed blobs.
  ui32 CompressUnused(ui64 time, ui64 max_size,
                      Clock::time_point deadline = Clock::time_point::max());

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
};

}  // namespace cache
}  // namespace dist_clang
