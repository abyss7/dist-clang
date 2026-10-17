#include <base/file/file.h>

#include <base/c_utils.h>

#include <copyfile.h>
#include <fcntl.h>
#include <sys/mman.h>

namespace dist_clang {
namespace base {

bool File::AdviseSequentialRead() {
  return fcntl(native(), F_RDAHEAD, 1) != -1;
}

void File::Preallocate(ui64 size) {
  fstore_t store = {
      F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0,
      static_cast<off_t>(size),
  };
  if (fcntl(native(), F_PREALLOCATE, &store) == -1) {
    store.fst_flags = F_ALLOCATEALL;
    fcntl(native(), F_PREALLOCATE, &store);
  }
}

void* File::MapForRead(ui64 size) {
  return mmap(nullptr, size, PROT_READ, MAP_PRIVATE, native(), 0);
}

bool File::CopyContentsInto(File& dst, ui64, String* error) {
  if (fcopyfile(native(), dst.native(), nullptr, COPYFILE_ALL) != 0) {
    GetLastError(error);
    return false;
  }
  return true;
}

// static
bool File::CloneContents(const Path&, const Path&) {
  // Unsupported yet: see |clonefile()|.
  return false;
}

}  // namespace base
}  // namespace dist_clang
