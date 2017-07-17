#include <base/file/file.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file_utils.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#if defined(OS_LINUX)
#include <sys/sendfile.h>
#elif defined(OS_MACOSX)
#include <copyfile.h>
#endif

namespace dist_clang {

namespace base {

File::File(const Path& path)
    : Data(open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
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

File::File(const Path& path, ui64 size)
    : Data([=] {
        // We need write-access even on object files after introduction of the
        // "split-dwarf" option, see
        // https://sourceware.org/bugzilla/show_bug.cgi?id=971
        const auto mode = mode_t(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        const auto flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
        const String tmp_path = path.string() + ".tmp";
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

  const String tmp_path = move_on_close_ + ".tmp";
  if (!Move(tmp_path, move_on_close_, error)) {
    Delete(tmp_path);
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
