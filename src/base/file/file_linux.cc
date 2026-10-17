#include <base/file/file.h>

#include <base/c_utils.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

bool File::AdviseSequentialRead() {
  return posix_fadvise(native(), 0, 0, POSIX_FADV_SEQUENTIAL) == 0;
}

void File::Preallocate(ui64 size) {
  posix_fallocate(native(), 0, size);
}

void* File::MapForRead(ui64 size) {
  return mmap64(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, native(),
                0);
}

bool File::CopyContentsInto(File& dst, ui64 size, String* error) {
  ui64 total_bytes = 0;
  while (total_bytes < size) {
    const ssize_t copied =
        sendfile(dst.native(), native(), nullptr, size - total_bytes);
    if (copied <= 0) {
      GetLastError(error);
      return false;
    }
    total_bytes += copied;
  }
  return true;
}

// static
bool File::CloneContents(const Path& src, const Path& dst) {
  const int src_fd = open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (src_fd == -1) {
    return false;
  }
  struct stat st;
  const int dst_fd =
      fstat(src_fd, &st) == -1
          ? -1
          : open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                 (st.st_mode | S_IWUSR) & 0777);
  const bool cloned = dst_fd != -1 && ioctl(dst_fd, FICLONE, src_fd) == 0;
  if (dst_fd != -1) {
    close(dst_fd);
  }
  close(src_fd);
  if (!cloned) {
    unlink(dst.c_str());
  }
  return cloned;
}

}  // namespace base
}  // namespace dist_clang
