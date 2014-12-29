#include <base/file/file.h>

#include <base/c_utils.h>
#include <base/string_utils.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace dist_clang {
namespace base {

File::File(const String& path) : Data(open(path.c_str(), O_RDONLY)) {
  if (!IsValid()) {
    GetLastError(&error_);
  }
}

ui64 File::Size(String* error) const {
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

#if defined(OS_LINUX)
  if (posix_fadvise(native(), 0, 0, POSIX_FADV_SEQUENTIAL) == -1) {
    GetLastError(error);
    return false;
  }
#elif defined(OS_MACOSX)
  if (fcntl(native(), F_RDAHEAD, 1) == -1) {
    GetLastError(error);
    return false;
  }
#endif

  auto size = Size();
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
  if (!Read(output, error)) {
    return false;
  }

  for (const char* skip : skip_list) {
    if (output->find(skip) != String::npos) {
      if (error) {
        error->assign("Skip-list hit: " + String(skip));
      }
      return false;
    }
  }

  output->assign(base::Hexify(output->Hash()));
  return true;
}

// static
bool File::Read(const String& path, Immutable* output, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Read(output, error);
}

// static
bool File::Hash(const String& path, Immutable* output,
                const List<Literal>& skip_list, String* error) {
  File file(path);

  if (!file.IsValid()) {
    file.GetCreationError(error);
    return false;
  }

  return file.Hash(output, skip_list, error);
}

}  // namespace base
}  // namespace dist_clang
