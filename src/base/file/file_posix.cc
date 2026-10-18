#include <base/file/file.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/path_utils.h>
#include <base/string_utils.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

File::File(const Path& path) : Data(open(path.c_str(), O_RDONLY)) {
  if (!IsValid()) {
    GetLastError(&error_);
    return;
  }

  if (!IsFile(path)) {
    error_ = path.string() + " is a directory";
    Handle::Close();
    return;
  }

  if (!AdviseSequentialRead()) {
    GetLastError(&error_);
    Handle::Close();
    return;
  }
}

// static
bool File::IsFile(const Path& path, String* error) {
  struct stat buffer;
  if (stat(path.c_str(), &buffer)) {
    GetLastError(error);
    return false;
  }

  return (buffer.st_mode & S_IFDIR) == 0;
}

// static
bool File::IsExecutable(const Path& path, String* error) {
  if (!IsFile(path, error)) {
    return false;
  }

  if (access(path.c_str(), X_OK)) {
    GetLastError(error);
    return false;
  }
  return true;
}

// static
bool File::Exists(const Path& path, String* error) {
  if (access(path.c_str(), F_OK)) {
    GetLastError(error);
    return false;
  }

  if (!IsFile(path, error)) {
    return false;
  }

  return true;
}

ui64 File::Size(String* error) const {
  DCHECK(IsValid());

  struct stat buffer;
  if (fstat(native(), &buffer)) {
    GetLastError(error);
    return 0;
  }

  return buffer.st_size;
}

bool File::Read(Immutable* output, String* error) {
  DCHECK(IsValid());

  if (!output) {
    return false;
  }

  auto size = Size();
  if (size == 0) {
    // TODO: write test on reading empty file.
    output->assign(Immutable());
    return true;
  }

  // The contents are copied, not mapped: the file may be rewritten in place
  // or truncated meanwhile - e.g. a header - then the mapping would change, or
  // the access to it would crash the process.
  String contents(size, '\0');
  ui64 total = 0;
  while (total < size) {
    const ssize_t bytes = pread(native(), contents.data() + total, size - total,
                                static_cast<off_t>(total));
    if (bytes == -1 && errno == EINTR) {
      continue;
    }
    if (bytes == -1) {
      GetLastError(error);
      return false;
    }
    if (bytes == 0) {
      // Truncated meanwhile.
      contents.resize(total);
      break;
    }
    total += bytes;
  }

  output->assign(Immutable(std::move(contents)));

  return true;
}

bool File::Hash(Immutable* output, const List<Literal>& skip_list,
                String* error) {
  DCHECK(IsValid());

  Immutable tmp_output;
  if (!Read(&tmp_output, error)) {
    return false;
  }

  for (const char* skip : skip_list) {
    if (tmp_output.find(skip) != String::npos) {
      if (error) {
        error->assign("Skip-list hit: " + String(skip));
      }
      return false;
    }
  }

  output->assign(base::Hexify(tmp_output.Hash()));
  return true;
}

bool File::CopyInto(const Path& dst_path, String* error) {
  DCHECK(IsValid());

  // Force unlinking of |dst|, since it may be hard-linked with other places.
  if (unlink(dst_path.c_str()) == -1 && errno != ENOENT) {
    GetLastError(error);
    return false;
  }

  const auto src_size = Size();
  File dst(dst_path, src_size);

  if (!dst.IsValid()) {
    dst.GetCreationError(error);
    return false;
  }

  const bool result = CopyContentsInto(dst, src_size, error);

  if (!dst.Close(error)) {
    return false;
  }

  return result;
}

// static
ui64 File::Size(const Path& path, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return 0;
  }

  return file.Size(error);
}

// static
bool File::Read(const Path& path, Immutable* output, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Read(output, error);
}

// static
bool File::Hash(const Path& path, Immutable* output,
                const List<Literal>& skip_list, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Hash(output, skip_list, error);
}

// static
bool File::Delete(const Path& path, String* error) {
  if (unlink(path.c_str()) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

// static
bool File::Write(const Path& path, Immutable input, String* error) {
  File dst(path, input.size());

  if (!dst.IsValid()) {
    GetLastError(error);
    return false;
  }

  size_t total_bytes = 0;
  int size = 0;
  while (total_bytes < input.size()) {
    size = write(dst.native(), input.data() + total_bytes,
                 input.size() - total_bytes);
    if (size <= 0) {
      break;
    }
    total_bytes += size;
  }

  if (!dst.Close(error)) {
    return false;
  }

  return total_bytes == input.size();
}

// static
bool File::Copy(const Path& src_path, const Path& dst_path, String* error) {
  File src(src_path);

  if (!src.IsValid()) {
    src.GetCreationError(error);
    return false;
  }

  return src.CopyInto(dst_path, error);
}

// static
bool File::Link(const Path& src, const Path& dst, String* error) {
  auto Link = [&src, &dst]() -> int {
    // The |link()| doesn't always dereference the symlinks, thus we use
    // |linkat()| which does for sure.
    return linkat(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(),
                  AT_SYMLINK_FOLLOW);
  };

  // Try to create hard-link at first.
  if (Link() == 0 ||
      (errno == EEXIST && unlink(dst.c_str()) == 0 && Link() == 0)) {
    return true;
  }

  return File::Copy(src, dst, error);
}

// static
bool File::Clone(const Path& src, const Path& dst, bool allow_hardlink,
                 CloneMode* mode, String* error) {
  // Use a unique temporary file in the same directory, so that the final
  // rename is atomic.
  static Atomic<ui64> counter(0);
  const Path tmp_path = dst.string() + ".dc-" + std::to_string(getpid()) +
                        "-" + std::to_string(counter++) + ".tmp";

  auto Finish = [&](CloneMode clone_mode) {
    const bool moved = Move(tmp_path, dst, error);
    // The rename does nothing, if |dst| is a hard link of the same file
    // already - and the temporary file stays.
    unlink(tmp_path.c_str());
    if (!moved) {
      return false;
    }
    if (mode) {
      *mode = clone_mode;
    }
    return true;
  };

  if (CloneContents(src, tmp_path)) {
    return Finish(CloneMode::REFLINK);
  }

  if (allow_hardlink && linkat(AT_FDCWD, src.c_str(), AT_FDCWD,
                               tmp_path.c_str(), AT_SYMLINK_FOLLOW) == 0) {
    return Finish(CloneMode::HARDLINK);
  }

  if (!Copy(src, tmp_path, error)) {
    unlink(tmp_path.c_str());
    return false;
  }
  return Finish(CloneMode::COPY);
}

// static
bool File::Move(const Path& src, const Path& dst, String* error) {
  if (rename(src.c_str(), dst.c_str()) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}

File::File(const Path& path, ui64 size)
    : Data([=] {
        // We need write-access even on object files after introduction of the
        // "split-dwarf" option, see
        // https://sourceware.org/bugzilla/show_bug.cgi?id=971
        const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        const auto flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
        const Path tmp_path = AppendExtension(path, "tmp"_l);
        return open(tmp_path.c_str(), flags, mode);
      }()),
      move_on_close_(path) {
  if (!IsValid()) {
    GetLastError(&error_);
    return;
  }

  // FIXME: we should respect umask somehow.
  DCHECK([&] {
    struct stat st;
    const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    return fstat(native(), &st) == 0 && (st.st_mode & mode) == mode;
  }());

  Preallocate(size);
}

bool File::Close(String* error) {
  Handle::Close();

  if (move_on_close_.empty()) {
    return true;
  }

  const Path tmp_path = AppendExtension(move_on_close_, "tmp"_l);
  if (!Move(tmp_path, move_on_close_, error)) {
    Delete(tmp_path);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
