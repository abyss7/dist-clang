#pragma once

#include <base/types.h>

namespace dist_clang {
namespace daemon {

// Checks if the file at |path| has the same contents as the cache file
// |cache_file| with the known hash |cache_hash| (as |base::File::Hash()|).
bool HasSameContents(const Path& path, const Path& cache_file,
                     const String& cache_hash);

// Checks if the file at |path| has the |contents|.
bool HasSameContents(const Path& path, Immutable contents);

// Keeps the previous version of an output aside, while the output is being
// rebuilt - to put it back, if the new one is the same. Then the output keeps
// its modification time, and the build system, that re-stats outputs, doesn't
// rebuild the dependent targets.
class PreviousOutput {
 public:
  explicit PreviousOutput(const Path& path);
  ~PreviousOutput();

  // Moves the output aside, if it exists.
  void Save();

  // Puts the previous output back, if the new one has the same contents.
  // Returns |true| if it did.
  bool RestoreIfSame();

  // Puts the previous output back - e.g. the compilation is canceled: the
  // output of the cache may be the same.
  void Restore();

 private:
  const Path path_, saved_path_;
  bool saved_ = false;
};

}  // namespace daemon
}  // namespace dist_clang
