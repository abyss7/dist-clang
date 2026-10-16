#pragma once

#include <base/types.h>

struct sqlite3;

namespace dist_clang {
namespace cache {

// Persistent records of the removed entries of the cache - the "ghosts": to
// tell, which removals were losses. The entry is found by the hash of its
// object file: when the same object is compiled again, the removed entry
// would have been a hit.
//
// The records are kept for a limited time - see |RemoveOlderThan()|.
class GhostIndex {
 public:
  enum Reason : ui32 {
    EVICTED = 1,    // To free the space for the new entries.
    FORGOTTEN = 2,  // By the user: with the tag, or without tags.
  };

  struct Ghost {
    String object;  // The hash of the object file.
    ui64 size = 0;  // The space, that the removal has freed.
    ui64 compilation_time = 0;  // in milliseconds, 0 if unknown.
    Reason reason = EVICTED;
    Vector<String> tags;
  };

  // The removed entry is needed again.
  struct Loss {
    Reason reason = EVICTED;
    ui64 compilation_time = 0;  // in milliseconds - of the new compilation.
    // For the evicted entries: how much larger cache would have kept it - the
    // size of the entries, that were evicted since it, with it.
    ui64 extra_size = 0;
    Vector<String> tags;
  };

  GhostIndex(const String& path, const String& name);
  ~GhostIndex();

  // The entry is removed at |time|.
  bool Add(const Ghost& ghost, ui64 time);

  // A new entry has the |object|. If it's of a removed entry, the loss is
  // counted - and the record of the removed entry is dropped: the entry exists
  // again. Returns |true| on a loss.
  bool Resurrect(const String& object, ui64 compilation_time, ui64 time);

  ui64 Count() const;
  List<Loss> GetLosses() const;

  // Removes the records of the removals and of the losses before |time|.
  bool RemoveOlderThan(ui64 time);
  // Returns the number of the removed records of the removals.
  ui64 Clear();

  bool BeginTransaction();
  bool EndTransaction();

  ui64 SizeOnDisk() const;

 private:
  ui64 EvictedSize() const;

  const String path_;
  sqlite3* db_ = nullptr;
};

}  // namespace cache
}  // namespace dist_clang
