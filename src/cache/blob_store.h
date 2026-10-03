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

  // The methods below are called only by the cleaner.

  // Counts the reference from an entry, that was used at |access_time|.
  void AddRef(const String& hash, ui64 access_time);
  void Touch(const String& hash, ui64 access_time);
  // Removes the blob without references.
  void RemoveRef(const String& hash);

  // Removes the blobs without references, that are created before |time| -
  // e.g. after a crash. Call after all references are added.
  ui32 RemoveUnreferenced(ui64 time);

  // Compresses the blobs, that aren't linked with other places, and weren't
  // used since |time|. Stops after |max_size| bytes. Returns the number of
  // the compressed blobs.
  ui32 CompressUnused(ui64 time, ui64 max_size);

  // The size on the disk, and if the blob is linked with other places - e.g.
  // with build directories, so its removal doesn't free the space.
  ui64 Size(const String& hash) const;
  bool IsShared(const String& hash) const;
  ui32 Refs(const String& hash) const;

  // The total size of the blobs, that are and aren't linked with other places.
  void GetTotalSize(ui64* own_size, ui64* shared_size) const;

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

  HashMap<String, Blob> blobs_;
};

}  // namespace cache
}  // namespace dist_clang
