#include <base/temporary_dir.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include <ftw.h>

namespace {

int Remove(const char* path, const struct stat* sb, int type, struct FTW*) {
  switch (type) {
    case FTW_F:
    case FTW_SL:
    case FTW_SLN:
      unlink(path);
      break;

    case FTW_D:
    case FTW_DP:
    case FTW_DNR:
      rmdir(path);
      break;
  }

  return 0;
}

}  // namespace

namespace dist_clang {
namespace base {

TemporaryDir::TemporaryDir() {
  char buf[] = "/tmp/clangd-XXXXXX";
  if (!mkdtemp(buf)) {
    GetLastError(&error_);
    return;
  }
  path_ = buf;
}

TemporaryDir::~TemporaryDir() {
  nftw(path_.c_str(), Remove, 4, FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
  DCHECK_O_EVAL(!rmdir(path_.c_str()) || errno == ENOENT);
}

}  // namespace base
}  // namespace dist_clang
