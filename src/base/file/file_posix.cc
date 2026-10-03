#include <base/file/file.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/path_utils.h>
#include <base/string_utils.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(OS_LINUX)
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#elif defined(OS_MACOSX)
#include <copyfile.h>
#endif

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

#if defined(OS_LINUX)
  if (posix_fadvise(native(), 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(&error_);
    Handle::Close();
    return;
  }
#elif defined(OS_MACOSX)
  if (fcntl(native(), F_RDAHEAD, 1) == -1) {
    GetLastError(&error_);
    Handle::Close();
    return;
  }
#endif
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

  auto flags = MAP_PRIVATE;
#if defined(OS_LINUX)
  flags |= MAP_POPULATE;
  void* map = mmap64(nullptr, size, PROT_READ, flags, native(), 0);
#elif defined(OS_MACOSX)
  void* map = mmap(nullptr, size, PROT_READ, flags, native(), 0);
#else
#pragma message "This platform doesn't support mmap interface!"
  void* map = MAP_FAILED;
#endif
  if (map == MAP_FAILED) {
    GetLastError(error);
    return false;
  }

  output->assign(Immutable(map, size));

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

  bool result = false;
#if defined(OS_LINUX)
  size_t total_bytes = 0;
  ssize_t size = 0;
  while (total_bytes < src_size) {
    size = sendfile(dst.native(), native(), nullptr, src_size - total_bytes);
    if (size <= 0) {
      break;
    }
    total_bytes += size;
  }
  result = (total_bytes == src_size);
#elif defined(OS_MACOSX)
  if (fcopyfile(native(), dst.native(), nullptr, COPYFILE_ALL) != 0) {
    GetLastError(error);
    return false;
  }
  result = true;
#else
#pragma message "Don't know how to make quick file copy on this platform!"
// TODO: implement fallback file copy.
#endif

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
#if defined(OS_LINUX)
    // Linux doesn't guarantee that |link()| do dereferences symlinks, thus
    // we use |linkat()| which does for sure.
    return linkat(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(),
                  AT_SYMLINK_FOLLOW);
#elif defined(OS_MACOSX)
    return link(src.c_str(), dst.c_str());
#else
#pragma message "This platform doesn't support hardlinks!"
    errno = EACCES;
    return -1;
#endif
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

#if defined(OS_LINUX)
  {
    const int src_fd = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (src_fd == -1) {
      GetLastError(error);
      return false;
    }
    struct stat st;
    const int dst_fd =
        fstat(src_fd, &st) == -1
            ? -1
            : open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   (st.st_mode | S_IWUSR) & 0777);
    const bool cloned = dst_fd != -1 && ioctl(dst_fd, FICLONE, src_fd) == 0;
    if (dst_fd != -1) {
      close(dst_fd);
    }
    close(src_fd);
    if (cloned) {
      return Finish(CloneMode::REFLINK);
    }
    unlink(tmp_path.c_str());
  }
#endif  // defined(OS_LINUX)

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

#if defined(OS_LINUX)
  if (posix_fallocate(native(), 0, size) == -1) {
    GetLastError(&error_);
    Handle::Close();
    return;
  }
#elif defined(OS_MACOSX)
  fstore_t store = {
      F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0,
      static_cast<off_t>(size),
  };

  if (fcntl(native(), F_PREALLOCATE, &store) == -1) {
    store.fst_flags = F_ALLOCATEALL;
    if (fcntl(native(), F_PREALLOCATE, &store) == -1) {
      GetLastError(&error_);
      Handle::Close();
      return;
    }
  }
#endif
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
