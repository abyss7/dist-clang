#include <base/file/file.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file_utils.h>
#include <base/string_utils.h>

#include STL_EXPERIMENTAL(filesystem)
#include STL(system_error)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#if defined(OS_LINUX)
#include <sys/sendfile.h>
#elif defined(OS_MACOSX)
#include <copyfile.h>
#endif

namespace dist_clang {

namespace {

bool GetStatus(const Path& path,
               std::experimental::filesystem::file_status* status,
               String* error) {
  CHECK(status);
  std::error_code ec;
  *status = std::experimental::filesystem::status(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return false;
  }
  return true;
}

Path AddTempEnding(const Path& path) {
  return base::AppendExtension(path, ".tmp");
}

}  // anonymous namespace

namespace base {

File::File(const Path& path)
    : Data(open(path.c_str(), O_RDONLY | O_CLOEXEC)), path_(path) {
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
  std::experimental::filesystem::file_status status;
  if (!GetStatus(path, &status, error)) {
    return false;
  }
  return std::experimental::filesystem::is_regular_file(status);
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
  std::experimental::filesystem::file_status status;
  if (!GetStatus(path, &status, error)) {
    return false;
  }
  return std::experimental::filesystem::exists(status) &&
         std::experimental::filesystem::is_regular_file(status);
}

ui64 File::Size(String* error) const {
  DCHECK(IsValid());

  return File::Size(path_, error);
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
  std::error_code ec;
  const auto file_size = std::experimental::filesystem::file_size(path, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
    return 0;
  }
  return static_cast<ui64>(file_size);
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
  std::error_code ec;
  if (!std::experimental::filesystem::remove(path, ec)) {
    if (error) {
      *error = ec.message();
    }
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
bool File::Move(const Path& src, const Path& dst, String* error) {
  std::error_code ec;
  std::experimental::filesystem::rename(src, dst, ec);
  if (ec) {
    if (error) {
      *error = ec.message();
    }
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
        const Path tmp_path = AddTempEnding(path);
        return open(tmp_path.c_str(), flags, mode);
      }()),
      path_(AddTempEnding(path)),
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

  const Path tmp_path = AddTempEnding(move_on_close_);
  if (!Move(tmp_path, move_on_close_, error)) {
    Delete(tmp_path);
    return false;
  }

  path_ = move_on_close_;
  return true;
}

}  // namespace base
}  // namespace dist_clang
