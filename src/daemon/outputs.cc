#include <daemon/outputs.h>

#include <base/const_string.h>
#include <base/file/file.h>

#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace daemon {

bool HasSameContents(const Path& path, const Path& cache_file,
                     const String& cache_hash) {
  struct stat st, cache_st;
  if (stat(path.c_str(), &st) == -1) {
    return false;
  }
  if (!cache_file.empty() && stat(cache_file.c_str(), &cache_st) == 0) {
    if (st.st_dev == cache_st.st_dev && st.st_ino == cache_st.st_ino) {
      return true;
    }
    // The cache file isn't compressed - the sizes of the same contents match.
    if (st.st_size != cache_st.st_size) {
      return false;
    }
  }
  if (cache_hash.empty()) {
    return false;
  }

  Immutable hash;
  return base::File::Hash(path, &hash) && hash.string_copy() == cache_hash;
}

bool HasSameContents(const Path& path, Immutable contents) {
  if (!base::File::Exists(path) || base::File::Size(path) != contents.size()) {
    return false;
  }

  Immutable file_contents;
  return base::File::Read(path, &file_contents) && file_contents == contents;
}

PreviousOutput::PreviousOutput(const Path& path)
    : path_(path), saved_path_(path.string() + ".dc-prev") {}

PreviousOutput::~PreviousOutput() {
  if (saved_) {
    unlink(saved_path_.c_str());
  }
}

void PreviousOutput::Save() {
  // The rename replaces the stale one.
  saved_ = rename(path_.c_str(), saved_path_.c_str()) == 0;
  if (!saved_) {
    unlink(saved_path_.c_str());
  }
}

void PreviousOutput::Delete() {
  unlink(path_.c_str());
  unlink(saved_path_.c_str());
}

void PreviousOutput::Restore() {
  if (saved_ && rename(saved_path_.c_str(), path_.c_str()) == 0) {
    saved_ = false;
  }
}

bool PreviousOutput::RestoreIfSame() {
  if (!saved_) {
    return false;
  }

  Immutable new_hash, old_hash;
  if (!base::File::Exists(path_) ||
      base::File::Size(path_) != base::File::Size(saved_path_) ||
      !base::File::Hash(path_, &new_hash) ||
      !base::File::Hash(saved_path_, &old_hash) || !(new_hash == old_hash)) {
    return false;
  }

  if (rename(saved_path_.c_str(), path_.c_str()) != 0) {
    return false;
  }
  saved_ = false;
  return true;
}

}  // namespace daemon
}  // namespace dist_clang
